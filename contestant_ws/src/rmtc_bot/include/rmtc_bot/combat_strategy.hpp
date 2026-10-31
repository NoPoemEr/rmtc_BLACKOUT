#pragma once
// CombatStrategy —— M3 第二阶段对抗策略
//   10/27 MVP：均衡规则层（B1/B3/B4/B5，docs/combat_design.md）
//   10/28 v2：决策层接入 + 战术优化（docs/决策设计.md §6 步骤 2；用户拍板
//             "写完策略代码→先优化→再跑 10 场实测"）
//   10/28 v2.1（晚，用户拍板批次四）：死亡修复 P1–P3
//             （低血撤离重写/撤离格质量/低血攻击降权，10 场回归验收）
//   10/28 v2.2（晚，自对弈 round1/round2 修复批次）：
//             R1 生存门（绝不为收头赔命，k_in 只数会反击的敌车）；
//             R2-B 低血回血路线（消费 LLM 校验结论的增益点位，破撤离死锁）。
//             注：R2 期间尝试过攻击倾向门（对称换血抑制）三个版本，官方陪练
//             回归全部掉伤害掉排名 → 撤销，保留 IsIsolated 换血；镜像 20/20
//             死锁由 R1 生存门 + R2-B 回血路线兜底（见 round2 分析）。
//   10/30 V7（用户拍板"人多聚堆就上前劝架、防御目标落井下石"+"任务/资源
//             分作为参战信念"）：R4-A 集火参与（替换 R2-C 多敌避让默认分支）
//             + 参战信念门（任务全清且资源饱和 或 战斗任务在身才主动找架）
//             + 防御中目标可打（半伤落井下石，其本 Tick 不反击）。
//   10/30 V8（V7 首场练习赛实测 + 用户拍板三条）：
//             R5-A 参战门补"资源采尽"信号（V7 实测全图资源 < 饱和线 → 门永远
//               关闭、0 集火 53 避让；改为"信念已知资源全消失且视野无资源"）；
//             R5-B 幻觉自适应（Illusion 方向反转实测 tick 45–53 漂到地图边缘：
//               回执方向 vs 发出方向相反即幻觉，emit 统一反向 move，attack 不反）；
//             R5-C 障碍遗忘（WorldSplit 临时墙被永久记忆 → 绕鬼墙 12 帧闭环
//               徘徊 116 tick；视野内消失的障碍从信念移除）；
//             R5-D 巡逻锚定修复（"在环上就推进"在绕行途中劫持锚定 → 仅到达
//               当前路点才推进；复位时在环上锚定当前环格）；
//             R5-E 敢死队（用户拍板"残血不用非要避战"：本命累计伤害 ≥
//               death_penalty+attack_damage 即回本，放宽 R1 生存门/低血换血门、
//               跳过低血避火、绕过参与门槛——打伤害死掉满状态复活，净赚）；
//             R5-F 参战锁存（用户拍板"不能确定真实地图、角落可能藏资源"：
//               采尽判定须绕环满一圈实证，成立即锁存；此后偶见资源由采集层
//               顺手捡，不把参战门关回去、不影响参战状态）。
//             R5-G 巡逻实证改覆盖率（V8 练习赛实测：顺序指针满圈被多敌避让
//               反复劫持，tick 470+ 才凑满、全场集火参与仅 1 次；踩过的环格
//               28/28 其实 tick 317 就齐了）——锁存条件改为"踩过的环格 ≥
//               patrol_lap_coverage 比例（默认 90%）"；多敌避让在环上时优先
//               沿环逃逸（距离差距 ≤1 内），不再与环形巡逻互相拉拽振荡。
//   10/30 V8.1（V8 练习赛复盘 + 用户两条反馈）：
//             R5-H 血量碾压参战（用户："血量明显比对方高却不敢打"——V8 实测
//               27 次多敌避让里 26 次我方 HP ≥ 2× 敌车最低血）：参战门补
//               "血量优势"信号——我方 ≥ 3×attack_damage 且 ≥ 2×最低血敌车、
//               ≥ 最高血敌车即主动参战（集火最低血/单敌追猎），任务/采集层
//               优先级更高不抢，R4-A 低血/生存/饱和安全门保留；
//             R5-I 巡逻沿环走 + 瞬移归环（用户："中间有段时间从地图中间横穿
//               过去而不是沿着环走"——V8 实测两机制：① 路点被敌占跳过后 BFS
//               从 (9,5) 直插 (12,10) 切腹地；② tick 135 死→136 直接满血
//               复活在出生点，死亡帧被跳过 → 重生就近归环失效，BFS 从角落
//               直插对岸路点横穿全图）：在环上沿环推进（顺/逆时针短弧），
//               位置瞬移 >1 格视同重生就近归环。
//   10/30 V8.2（用户两条新反馈；8.1 尚未实测）：
//             R5-J 血高即追（用户："血量比对面高就应该追着打了，没必要做
//               那么高的限制"）：hp_dominant 砍掉 R5-H 的 3×damage 下限与
//               2×最低血条件，改为"我方血 > 视野内最强敌车"即参战；R4-A
//               逼近门槛改为"血高于目标"（原 ≥2×damage），R3-A 追猎门槛改为
//               "血不低于对方"（原 ≥3×damage）。R1 生存门/威胁步进安全门
//               保留；饱和门后由 R5-N 撤掉（超 500 仍照常打，V8.6c 复核）。
//             R5-K 方环巡逻（用户："巡逻轨迹做正方形吧，不需要圆角"）：
//               BuildPatrolRing 去掉四角切角，改直角矩形环（上→右→下→左
//               顺时针），尺寸仍由地图 w/h 推导、非写死。
//
// v2 漏斗（决定顺序与参数全部来自 DecisionParams，语义层无规则数值字面量）：
//   0. 同帧幂等缓存（恢复重发同 Tick 输出逐字节一致，selftest 10）
//   1. 信念更新 decision_model::update_belief（障碍记忆/饱和度/出生点，每 Tick）
//   1.5 LLM 结论 parse/validate（R2-B；event_text 原文不本地解析）
//   2. 硬红线：阵亡 → wait；无位置/地图 → wait（selftest 1/2）
//   3. 攻击判定（selftest 4–8 + 14/15 + 24 + 26/27/33）：直线 1–2 格、非保护
//      目标（V7/R4-A：防御中照打——半伤落井下石，其本 Tick 不反击）、方向内
//      无更近敌车遮挡；可击杀（hp ≤ attack_damage，防御目标按半伤折算）优先；
//      我方对抗分已饱和 → 只打可击杀（击杀仍扣对手 D，超 500 部分照算排名）；
//      低血（< 2×attack_damage）第二遍只打严格领先一枪的目标（V6：取消 P3
//      整块跳门——低血不躺平，但必输换血不送头）；
//      R1 生存门：k_in×attack_damage ≥ 我方 HP → 攻击判定整块跳过；
//      R2：第二遍保留残血/严格领先一枪/孤立目标换血（攻击倾向门已试已撤）；
//      R3-A（10/28 晚，用户拍板"该激进就大胆，主动寻求换血"）：第二遍②
//      放宽为同血即打（落后仍不打）；满血游走单敌追猎（selftest 37–39）
//   4. 防御保命（selftest 17/21）：本 Tick 直线火力不防御必死、防御减半可活
//      → defend；低血受威胁且无合法撤离格 → defend（保守防裸挨打）
//   5. 低血避火（V6，10/29 用户定调"低血不能见人就跑、天使等不到"）：低血
//      不再有跨 Tick 撤离态——仅在真实威胁（枪线下 k_in≥1 或 ≥2 车近身）时
//      撤离评分（最小枪线 → 最大最小敌距 → 开放邻居 → 远离梯度），威胁解除
//      立即恢复任务/采集/巡逻；低血步进有"不增加枪线威胁"安全门；R2-B 回血
//      路线保留（天使恩泽公告时优先，selftest 16/18 + 22/23）
//   6. 采集：委托组合的 CollectStrategy（B1 方案 b；撞墙学障碍/卡住换目标）
//   7. 任务点：reach 类 mission 用信念障碍 BFS 首步推进（B4 顺路）；我方资源
//      分已饱和 → 任务优先于采集（selftest 19）
//   8. 游走兜底：环形巡逻（10/29 V5，用户拍板"金边银肚皮草角"；V6 修复脱环
//      吸附）——距地图边缘 D 格的圆角方形环（四角切角远离出生/复活点），
//      顺时针沿环巡逻；脱环（采集/避火）保持环序推进、仅重生时就近归环；
//      环不可用（极小图/全被堵）回退旧中心游走；移动被挡方向轮换
//      （selftest 13/20/41/42）；主动找架挂 V7/R4-A 参战信念门（10/30 用户拍板
//      "任务/资源分作为参战信念"）：存在未完成 effective_damage 战斗任务 或
//      （任务全清且资源饱和）才主动参战；参战开且视野 ≥2 敌 → 集火参与最低血
//      目标（防御中照打/落井下石，selftest 47–56），参战关 → R2-C 多敌避让；
//      恰 1 敌且参战开且满血有余量（≥ 3×attack_damage）、未饱和 → R3-A 追猎
//      （selftest 38/39）
//   LLM 结论（llm 参数）暂不使用——AI 资格由 bot_node 的 llm_brain 独立满足；
//   combat_llm_guide 意图翻译与事件应对按 docs/决策设计.md §6 步骤 3 接入。
//
// 设计依据与验收标准：docs/combat_design.md（§2 决策漏斗、§5.5 TDD 场景）与
// docs/决策设计.md（§1–§3 计分/信念/效用）。测试台：src/combat_selftest.cpp
// —— 不依赖裁判/ROS/LLM；`ros2 run rmtc_bot combat_selftest` 应 73/73 全绿。
//
// 接口契约（strategy.hpp 已注释）：decide 每 Tick 一次、禁阻塞、同帧幂等；
// on_new_session 清空全部记忆；阶段二红线动作白名单 + 保护期不攻击 + 阵亡 wait。

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rmtc_bot/collect_strategy.hpp"
#include "rmtc_bot/decision_model.hpp"
#include "rmtc_bot/llm_conclusion.hpp"
#include "rmtc_bot/strategy.hpp"

namespace rmtc_bot {

class CombatStrategy : public Strategy {
 public:
  CombatStrategy();  // 加载可选决策参数（默认=规则标准配置，见 decision_model）

  ActionChoice decide(const Perception &p, const LlmResult *llm) override;
  void on_new_session(const std::string &session_id) override;
  const char *name() const override { return "combat"; }

 private:
  CollectStrategy collector_;  // 组合复用采集内核（B1 方案 b）

  // 决策内核（10/28 v2 接入）：参数化规则快照 + 信念层
  DecisionParams params_;
  Belief belief_;

  // LLM 结论缓存（R2-B，10/28 自对弈 round2）：llm_brain 最近一次成功结论
  // 经 parse/validate 后存这里；低血回血路线消费 event_plan（avoid=false）。
  // 解析失败沿用上一次有效结论；换场复位（不跨场沿用旧事件点位）。
  bool has_conclusion_ = false;
  LlmConclusion last_conclusion_;

  // 游走方向记忆（中心被挡时轮换，selftest 20）
  std::string wander_dir_;

  // 环形巡逻状态（10/29 V5，用户拍板"金边银肚皮草角"）：距地图边缘 D 格的
  // 圆角方形环（四角切角避开出生/复活危险区），顺时针跟环走；换图/换场复位。
  // V6（10/29）：脱环不再吸附最近路点（会造成左上角徘徊不回环），仅真正重生
  // 时就近归环，其余情况沿环持续推进。
  int patrol_idx_ = 0;
  int patrol_w_ = 0;
  int patrol_h_ = 0;
  // R6-C（10/31 巡逻仿真发现）：邻环格被障碍/敌车占住时进入"绕行锁存"，
  // 直到抵达当前目标路点才恢复环上步进——否则 BFS 绕行的第一步落回另一个
  // 环格后，下一 Tick 的环序步进会把车推回原格，形成 A↔B 两格死循环
  // （仿真：环格 30% 被切断时 471/500 步是反向，覆盖 73%）。
  bool ring_detour_ = false;

  // 重生检测（V6 环形巡逻就近归环用）：上一 Tick 阵亡、本 Tick 复活
  bool alive_prev_ = true;

  // R5-I 瞬移检测（10/30 V8.1 实测）：上一 Tick 存活位置；本 Tick 位置跳变
  // >1 格 = 裁判跳过死亡帧直接复活/传送 → 视同重生（就近归环 + 本命基线复位）
  int last_x_ = -1;
  int last_y_ = -1;

  // R5-B 幻觉自适应（10/30 V8）：上一 Tick move 回执方向与发出方向相反 →
  // 幻觉生效，emit 统一反向发出 move（attack/defend 不受幻觉影响）。
  bool illusion_ = false;
  std::string last_emitted_dir_;  // 上一 Tick 实际发出的 move 方向（回执比对）

  // R5-E 敢死队（10/30 V8，用户拍板"残血不用非要避战"）：本命累计伤害
  // ≥ death_penalty + attack_damage（80）即回本——死亡只扣 60，回本后放宽
  // R1 生存门/低血换血门、跳过低血避火、绕过参与门槛，主动换伤害。
  bool life_baseline_set_ = false;
  double life_baseline_ = 0.0;

  // R5-F/R5-G 参战锁存（10/30 V8，用户拍板"不能确定真实地图、角落可能藏资源"）：
  // 资源侧"采尽"判定必须有一轮巡逻实证，且一旦成立即锁存——此后偶见资源由
  // 采集层顺手捡（采集优先级本就高于游走），不再把参战门关回去、不影响参战
  // 状态。R5-G（V8 练习赛实测）：实证从"顺序指针绕回环首"改为"踩过的环格
  // 覆盖率 ≥ patrol_lap_coverage"——顺序满圈被多敌避让劫持拖到 tick 470+，
  // 而踩格集合 tick 317 已覆盖全环（28/28）。
  std::set<std::pair<int, int>> ring_visited_;  // 本场踩过的环格（换图清空）
  bool economy_latched_ = false;  // 资源侧锁存（本场永久开门）

  // R5-P 追击纪律（10/31，用户："持续追击的 tick 要有限制，但能把对面压进角落
  // 就追到死"）：按目标位置记账——单次追击 tick 数 / 历史最近 BFS 距离 /
  // 距离停滞 tick / 放弃后的冷却。封角（可逃格 ≤ 阈值）或可一枪收头时不受限。
  int pursue_key_x_ = -1;      // 记账目标的绝对坐标（换目标即复位）
  int pursue_key_y_ = -1;
  int pursue_ticks_ = 0;       // 本次追击已用 tick
  int pursue_best_d_ = -1;     // 本次追击历史最近 BFS 距离
  int pursue_stall_ = 0;       // 距离没缩短的累计 tick
  int pursue_cool_ = 0;        // 放弃后的冷却剩余 tick
  bool last_was_pursuit_ = false;  // 上一 Tick 决策是否为追击（预算连续累计用）

  // 同帧幂等缓存（恢复重发同 Tick 输出逐字节一致，selftest 10）
  bool has_cached_ = false;
  int last_tick_ = -2;
  ActionChoice cached_;
};

}  // namespace rmtc_bot
