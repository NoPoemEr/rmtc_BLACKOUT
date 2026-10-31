#pragma once
// CollectStrategy —— M2 第一阶段采集策略（10/25 实现）
//
// 设计与验收标准：docs/collect_design.md（§0 验收标准 / §2 记忆地图 / §3 最近优先
// 贪心 + 三态状态机 / §4 每帧重算 BFS / §5 阶段一红线）。
// 测试台：src/collect_selftest.cpp —— §6.5 的 29 组场景，不依赖裁判、不依赖 ROS；
// `ros2 run rmtc_bot collect_selftest` 应全部转绿。
//
// 实现要点（与 collect_design.md 一一对应）：
//   - 记忆地图：资源按 id 记忆（含最后见到的 Tick）+ 障碍格集合，尺寸随
//     p.map_width/map_height，绝不硬编码 18×18
//   - 删除记忆唯一条件：站在资源格上且本帧看不到它（被我们采走）；
//     得分上涨绝不触发批量删除（10/21 线上坑，第 13 组回归用例）
//   - 撞墙回执（move 未动成 + 有效方向）→ 把该方向格记为障碍（撞出来的墙也是信息）；
//     非法方向一律不学习（L20）
//   - 目标选择：无资源任务时所有记忆资源 BFS 最近优先（§0.1 模拟数据）；
//     阶段二有 normal/high/resource_value 任务未完成时，按任务类型加显式
//     等效距离折扣（高价值任务未完成 → 高价值资源可多追约 10 步）
//   - 卡住判定：只统计 move 且未动成的回执（wait 回执不算），连续 3 帧 → 换目标，
//     且换目标排除刚放弃的目标（L21）
//   - 每帧重算 BFS 只取第一步方向；不可达 → 换目标；全部不可达 → 前沿探索
//     （10/31 R6-A：旧实现直接 wait，密障碍图实测 100 Tick 只拿 8/40 分）；
//     站在目标格上 → wait 等结算（L18，不发方向为空的 move）
//   - 前沿探索：记忆"已见格"（视野 ±4 的 9×9 全部格子），在已见可走格上 BFS，
//     选「9×9 新见格数 − 距离」最大的落点；只走已见格，探索不会撞墙
//   - 红线：只发 move/wait；alive=false → wait；position_reason 出现 → 清 MOVING
//   - 幂等：同 Tick 重复调用返回缓存动作（恢复重发同 Tick 输出逐字节一致）
//
// 接口契约（strategy.hpp 已注释，这里只划重点）：
//   - decide(p, llm)：每 Tick 一次、禁止阻塞；llm 参数阶段一忽略
//   - on_new_session()：换场清空记忆地图/目标状态
//   - 本类已在 strategy.cpp 登记：decision_type=collect 即创建本策略

#include <map>
#include <set>
#include <string>
#include <utility>

#include "rmtc_bot/strategy.hpp"

namespace rmtc_bot {

class CollectStrategy : public Strategy {
 public:
  // explore_enabled：是否启用 R6-A 前沿探索兜底。
  //   - 阶段一（decision_type=collect）默认 true：无已知资源必须找，不能 wait；
  //   - 阶段二（CombatStrategy 组合复用）构造时传 false，V8.6 起战斗层每 Tick
  //     按"资源未满/资源任务未完成且敌车不在 2 格内"打开；饱和/采尽/贴脸关闭。
  //     10/31 改探索时 combat_selftest 35/36/43 当场红：采集一旦总能给出 move，
  //     会抢在环形巡逻/追猎之前——所以开关必须由战斗层按态势控制。
  explicit CollectStrategy(bool explore_enabled = true)
      : explore_enabled_(explore_enabled) {}

  ActionChoice decide(const Perception &p, const LlmResult *llm) override;
  void on_new_session(const std::string &session_id) override;
  const char *name() const override { return "collect"; }

  // 阶段二逐 Tick 开关（10/31 V8.6 资源优先）：
  //   - set_explore_enabled：资源未拿满时允许无资源也探索（否则回 wait 交回战斗层）；
  //   - set_dynamic_blockers：E_BLOCKED 可能是敌车占位/幻术反转，不能一律学成墙；
  //   - set_illusion：幻术生效期间有效方向与意图相反，本次回执不学障碍。
  void set_explore_enabled(bool on) { explore_enabled_ = on; }
  void set_dynamic_blockers(bool on) { dynamic_blockers_ = on; }
  void set_illusion(bool on) { illusion_ = on; }

  // 已见格覆盖率（0–1）：战斗层判断"整图扫完没有"用，避免只绕环就宣布资源采尽。
  double explored_fraction(int w, int h) const;

 private:
  // ---- 记忆状态（collect_design.md §2 建议的落点）----
  struct Res {
    int x = 0;
    int y = 0;
    int last_seen_tick = 0;  // 最后出现在视野里的 Tick（诊断/日志用）
    int value = 10;           // 资源价值（普通 10 / 高价值 20；任务权重用）
  };
  std::map<std::string, Res> res_;            // id → 已知资源（id 有序，tie-break 确定）
  std::set<std::pair<int, int>> obstacles_;   // 已知障碍格（视野看到 + 撞墙学到）
  std::set<std::pair<int, int>> seen_;        // 已见格（视野 9×9 覆盖过的格）
  std::string target_;                        // 当前目标资源 id（空 = IDLE）
  std::string excluded_target_;               // 卡住换目标时本轮排除的资源 id（L21）
  int stuck_ = 0;                             // 连续"move 且未动成"回执帧数
  int last_tick_ = -2;                        // 上一帧 tick（同帧幂等去重）
  bool has_cached_ = false;
  bool explore_enabled_ = true;               // R6-A 探索兜底开关（阶段二按需开）
  bool dynamic_blockers_ = false;             // 阶段二：动态阻挡不学成墙
  bool illusion_ = false;                     // 本帧幻术生效（有效方向被反转）
  ActionChoice cached_;                       // 同帧幂等缓存
};

}  // namespace rmtc_bot
