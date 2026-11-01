// collect_selftest.cpp —— CollectStrategy 离线自检（30 组场景，不依赖 ROS/裁判）
//
// 第 13 组是 10/21 线上 stage1 实测抓出的真实 bug 回归用例（得分上涨误删视野外记忆）。
// 第 14–16 组是 10/27 静态审查 L18/L20/L21 修复的回归用例。
//
// 用法：ros2 run rmtc_bot collect_selftest
// 场景清单：docs/collect_design.md §6.5（每组的构造要点与断言都在那）。
//
// 断言全部按"BFS 邻居顺序无关"设计，实现时无需回头调整用例：
//   - 直线场景：起点到目标的最短路径唯一 → 断言具体方向（right/up）
//   - 绕行/撞墙/不回头场景：断言 direction != 某方向（任何邻居顺序都成立）
//   - 卡住换目标：备选资源放正上方（唯一最短路径）→ 断言 up
//
// 注意：CollectStrategy 尚未实现时（10/25 开工前）运行本自检为 9 红 4 绿
// —— 场景 1/9/10/11 的断言 wait 即满足（占位实现），其余红灯属预期 TDD 红灯；
// 策略实现正确后应全部转绿。

#include <cstdio>
#include <string>
#include <vector>

#include "rmtc_bot/collect_strategy.hpp"

using rmtc_bot::ActionChoice;
using rmtc_bot::CollectStrategy;
using rmtc_bot::Entity;
using rmtc_bot::Perception;

namespace {

int g_fail = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
      ++g_fail;                                                            \
    }                                                                      \
    std::fflush(stdout);                                                   \
  } while (0)

// CHECK_C：断言失败时顺带打印该 ActionChoice 的实际输出（action/direction/reason），
// 实现日调试不必先加打印就能看到"策略实际返回了什么"。
#define CHECK_C(cond, c)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  [FAIL] %s:%d  %s  (实际: action=%s dir=%s reason=%s)\n", \
                  __FILE__, __LINE__, #cond, (c).action.c_str(),             \
                  (c).direction.c_str(), (c).reason.c_str());                \
      ++g_fail;                                                              \
    }                                                                        \
    std::fflush(stdout);                                                     \
  } while (0)

// CHECK_CC：幂等/对比类断言，失败时两个 ActionChoice 都打印。
#define CHECK_CC(cond, a, b)                                                 \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  [FAIL] %s:%d  %s  (实际1: action=%s dir=%s | "          \
                  "实际2: action=%s dir=%s)\n",                              \
                  __FILE__, __LINE__, #cond, (a).action.c_str(),             \
                  (a).direction.c_str(), (b).action.c_str(),                 \
                  (b).direction.c_str());                                    \
      ++g_fail;                                                              \
    }                                                                        \
    std::fflush(stdout);                                                     \
  } while (0)

// ---- 场景构造辅助（实体直填绝对坐标，跳过 9×9 图解析层：只测策略）----

Entity Res(int x, int y, const std::string &id, int value = 10) {
  Entity e;
  e.type = value >= 20 ? "resource_high" : "resource_normal";
  e.id = id;
  e.has_absolute = true;
  e.abs_x = x;
  e.abs_y = y;
  e.has_value = true;
  e.value = value;
  return e;
}

Entity Obs(int x, int y) {
  Entity e;
  e.type = "obstacle";
  e.has_absolute = true;
  e.abs_x = x;
  e.abs_y = y;
  return e;
}

Entity RobotEntity(int x, int y, const std::string &team) {
  Entity e;
  e.type = "robot";
  e.team = team;
  e.has_absolute = true;
  e.abs_x = x;
  e.abs_y = y;
  return e;
}

// 基础帧：18×18 地图、本车绝对坐标、给定实体与 tick/session
Perception Frame(int tick, const std::string &session, int x, int y,
                 const std::vector<Entity> &entities = {}) {
  Perception p;
  p.tick = tick;
  p.session_id = session;
  p.map_width = 18;
  p.map_height = 18;
  p.self.has_position = true;
  p.self.x = x;
  p.self.y = y;
  p.self.has_alive = true;
  p.self.alive = true;
  p.entities = entities;
  return p;
}

// 上一 Tick 回执（撞墙/未移动等）：默认 move 且 moved=false
void SetReceipt(Perception &p, bool moved, const std::string &dir = "",
                const std::string &error = "") {
  p.self.has_last_action = true;
  p.self.last_tick = p.tick - 1;
  p.self.last_action = "move";
  p.self.last_moved = moved;
  if (!dir.empty()) {
    p.self.has_last_direction = true;
    p.self.last_effective_direction = dir;
  }
  if (!error.empty()) {
    p.self.has_last_error = true;
    p.self.last_error = error;
  }
}

// 资源任务（阶段二固定任务：2 普通 + 1 高价值 + reach）
rmtc_bot::Mission ResourceTask(const std::string &id, const std::string &type) {
  rmtc_bot::Mission m;
  m.id = id;
  m.type = type;
  m.target = 1;
  return m;
}

// ---- 场景清单 ----

void Test01_EmptyPerception() {
  std::printf("[TEST] 1 空感知不崩溃 → wait\n");
  CollectStrategy s;
  Perception p;  // 全默认：tick=-1、无位置、无地图
  ActionChoice c = s.decide(p, nullptr);  // llm=nullptr 也必须安全
  CHECK_C(c.action == "wait", c);
}

void Test02_NearestStraight() {
  std::printf("[TEST] 2 直线取最近资源 → right\n");
  CollectStrategy s;
  Perception p = Frame(10, "s", 5, 5, {Res(7, 5, "A"), Res(1, 1, "B")});
  ActionChoice c = s.decide(p, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "right", c);  // A 距 2、B 距 8；去 A 第一步唯一
}

void Test03_MemoryOutOfSight() {
  std::printf("[TEST] 3 视野外靠记忆 → 仍朝 A\n");
  CollectStrategy s;
  Perception f1 = Frame(10, "s", 5, 5, {Res(7, 5, "A")});
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "move" && c1.direction == "right", c1);

  // 本车左移出视野（不能在 9×9 内——窗口内看不到就是确定没了，见第 23 组）
  Perception f2 = Frame(11, "s", 2, 5);
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "right", c2);  // 记忆中 A 在 (7,5)，唯一最短路径
}

void Test04_ValueDoesNotDisturb() {
  std::printf("[TEST] 4 价值不干扰最近优先 → 朝近的普通资源\n");
  CollectStrategy s;
  Perception p = Frame(10, "s", 5, 5,
                       {Res(5, 3, "near", 10), Res(10, 5, "far", 20)});
  ActionChoice c = s.decide(p, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up", c);  // 近的普通资源在正上方（距 2）；高价值在远处（距 5）
}

void Test05_ObstacleDetour() {
  std::printf("[TEST] 5 障碍绕行 → 不穿墙（direction != right）\n");
  CollectStrategy s;
  Perception p = Frame(10, "s", 5, 5, {Obs(6, 5), Res(8, 5, "A")});
  ActionChoice c = s.decide(p, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction != "right", c);  // (6,5) 是墙，BFS 必绕（第一步 up 或 down）
}

void Test06_BlockedLearnsObstacle() {
  std::printf("[TEST] 6 撞墙学到障碍 → 下一帧不重蹈\n");
  CollectStrategy s;
  Perception f1 = Frame(10, "s", 5, 5, {Res(8, 5, "A")});
  (void)s.decide(f1, nullptr);  // 第一帧朝 right 撞墙（断言由 f2 覆盖）

  // f2：位置未变，回执说右方被挡。注意 entities 里没有 obstacle 实体
  // —— 就是要验证"撞出来的墙也是信息"（只靠回执学到障碍）。
  Perception f2 = Frame(11, "s", 5, 5, {Res(8, 5, "A")});
  SetReceipt(f2, /*moved=*/false, /*dir=*/"right", /*error=*/"E_BLOCKED");
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction != "right", c);
}

void Test07_StuckSwitchTarget() {
  std::printf("[TEST] 7 卡住换目标 → 第 5 帧朝备选资源 B\n");
  CollectStrategy s;
  // A(7,5) 被四面障碍围死：(6,5)(8,5)(7,4)(7,6)，BFS 无路；B(5,2) 可达且更远
  // （A 距 2、B 距 3：最近优先必然先选 A，卡住后必须换 B）。
  auto scene = {Res(7, 5, "A"), Res(5, 2, "B"),
                Obs(6, 5), Obs(8, 5), Obs(7, 4), Obs(7, 6)};
  for (int i = 0; i < 4; ++i) {
    Perception f = Frame(10 + i, "s", 5, 5, scene);
    if (i > 0) SetReceipt(f, /*moved=*/false);  // 一直没动成
    (void)s.decide(f, nullptr);
  }
  Perception f5 = Frame(14, "s", 5, 5, scene);
  SetReceipt(f5, /*moved=*/false);
  ActionChoice c = s.decide(f5, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up", c);  // B 在正上方，唯一最短路径
}

void Test08_CollectedRemoved() {
  std::printf("[TEST] 8 采集后删除记忆 → 不回头，转向下一个\n");
  CollectStrategy s;
  Perception f1 = Frame(10, "s", 5, 5, {Res(6, 5, "A"), Res(5, 3, "B")});
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "move" && c1.direction == "right", c1);  // 朝 A（唯一最短路径）

  Perception f2 = Frame(11, "s", 6, 5, {Res(5, 3, "B")});  // 已踩到 A：A 消失
  f2.score.resource_value = 10;  // 得分上涨 = 采集成功
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction != "right", c2);  // 不回头；B 在左上，第一步 up 或 left
}

void Test09_NewSessionClears() {
  std::printf("[TEST] 9 换场清空 → 无旧目标残留\n");
  CollectStrategy s;
  Perception f1 = Frame(10, "s1", 5, 5, {Res(7, 5, "A")});
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s1", 6, 5, {Res(7, 5, "A")});
  (void)s.decide(f2, nullptr);  // 记忆里已有目标 A

  s.on_new_session("s2");
  // 新场次：B(5,1) 在正上方。旧目标 A(7,5) 距 2 更近——若没清干净，会朝 right；
  // 清干净后最近的是 B，第一步唯一 up。10/31 R6-A 起"无资源"会探索，
  // 所以这里必须放一个资源，才能把断言钉在"旧记忆是否残留"上。
  Perception p3 = Frame(1, "s2", 5, 5, {Res(5, 1, "B")});
  ActionChoice c = s.decide(p3, nullptr);
  CHECK_C(c.action == "move" && c.direction == "up", c);
}

void Test10_Idempotent() {
  std::printf("[TEST] 10 幂等 → 同帧两次 decide 完全一致\n");
  CollectStrategy s;
  Perception p = Frame(10, "s", 5, 5, {Res(7, 5, "A"), Res(1, 1, "B")});
  ActionChoice c1 = s.decide(p, nullptr);
  ActionChoice c2 = s.decide(p, nullptr);
  CHECK_CC(c1.action == c2.action, c1, c2);
  CHECK_CC(c1.direction == c2.direction, c1, c2);
  CHECK_CC(c1.reason == c2.reason, c1, c2);  // 恢复重发同 Tick 时输出逐字节一致
}

void Test11_Stage1Whitelist() {
  std::printf("[TEST] 11 阶段一白名单 → 永不 attack/defend\n");
  CollectStrategy s;
  for (int i = 0; i < 3; ++i) {
    Perception f = Frame(10 + i, "s", 5, 5,
                         {RobotEntity(6, 5, "B"), Res(5, 3, "R")});
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action != "attack", c);
    CHECK_C(c.action != "defend", c);
    CHECK_C(c.action == "move" || c.action == "wait", c);
    if (c.action == "move") {
      CHECK_C(c.direction == "up" || c.direction == "down" ||
              c.direction == "left" || c.direction == "right", c);
    }
  }
}

void Test12_DeadAndRespawn() {
  std::printf("[TEST] 12 阵亡 → wait；重生 → 清状态重选\n");
  CollectStrategy s;
  Perception f1 = Frame(10, "s", 5, 5, {Res(7, 5, "A")});
  f1.self.alive = false;  // 阵亡
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "wait", c1);

  Perception f2 = Frame(11, "s", 3, 3, {Res(5, 3, "A")});  // 重生在 (3,3)，A 在正右
  f2.self.has_position_reason = true;
  f2.self.position_reason = "respawn";
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "right", c2);  // 清 MOVING 后重选：去 A 第一步唯一
}

void Test13_ScoreUpKeepsOutOfSight() {
  std::printf("[TEST] 13 采集不误删视野外记忆 → 继续朝 B\n");
  CollectStrategy s;
  Perception f1 = Frame(10, "s", 5, 5, {Res(6, 5, "A"), Res(1, 1, "B")});
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "move" && c1.direction == "right", c1);  // 先朝 A

  // f2：已踩到 A（A 消失、resource_value 上涨），B 在远处视野外、不在 entities。
  // 正确行为：只删 A（站在其格上），B 保留记忆继续导航。
  // 线上实测教训（10/21）：若按"得分上涨"删除所有视野外资源，车会失去全部记忆、
  // 在角落 wait 到结束（真实环境 90/200 败北；修复后 40/40）。
  Perception f2 = Frame(11, "s", 6, 5, {});
  f2.score.resource_value = 10;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "up" || c2.direction == "left", c2);  // 去 B 的最短路径第一步
}

// 10/27 审查 L18 回归：站在目标格且资源仍可见（本帧结算）→ 直接 wait，
// 不发方向为空的 Move("")（此前靠 bot_node 兜底降级，白白浪费 1 Tick）。
void Test14_StandingOnTargetWaits() {
  std::printf("[TEST] 14 站在目标格上 → wait 等结算（不发空方向 move）\n");
  CollectStrategy s;
  Perception f1 = Frame(10, "s", 5, 5, {Res(6, 5, "A")});
  (void)s.decide(f1, nullptr);  // 选 A 为目标，朝 right

  Perception f2 = Frame(11, "s", 6, 5, {Res(6, 5, "A")});  // 已站上 A，A 仍可见
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "wait", c);  // 等本帧结算，下一帧 A 消失即删除记忆
}

// 10/27 审查 L20 回归：非法 last_effective_direction 一律不学习——
// 否则 else 链会把"自己当前格"插入障碍集，永久污染地图（后续路线必须绕行）。
void Test15_IllegalDirectionNotLearned() {
  std::printf("[TEST] 15 非法回执方向不学障碍 → 不污染自己当前格\n");
  CollectStrategy s;
  // f1: 选 A(6,5)（距 1）为目标，朝 right
  Perception f1 = Frame(10, "s", 5, 5, {Res(6, 5, "A")});
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "move" && c1.direction == "right", c1);

  // f2: 回执方向非法（up_left）→ 不得把 (5,5) 学成墙
  Perception f2 = Frame(11, "s", 5, 5, {Res(6, 5, "A")});
  SetReceipt(f2, /*moved=*/false, /*dir=*/"up_left");
  (void)s.decide(f2, nullptr);

  // f3: 站上 A（仍可见，本帧结算）→ wait
  Perception f3 = Frame(12, "s", 6, 5, {Res(6, 5, "A")});
  (void)s.decide(f3, nullptr);

  // f4: A 已采走；B(4,5) 在左两格——最近路径必须穿过 (5,5)。
  // 若 (5,5) 被污染为墙：BFS 只能绕行，第一步是 up；正确行为第一步是 left。
  Perception f4 = Frame(13, "s", 6, 5, {Res(4, 5, "B")});
  ActionChoice c = s.decide(f4, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "left", c);
}

// 10/27 审查 L21 回归：卡住 K=3 换目标必须换到"不同"目标，
// 不得重选同一目标陷入原地磨蹭循环。
void Test16_StuckRepicksDifferentTarget() {
  std::printf("[TEST] 16 卡住换目标 → 排除刚放弃的目标\n");
  CollectStrategy s;
  // A(7,5) 距 2、B(3,5) 距 2，同距 tie-break 按 id：A 优先。
  // 回执永远"move 未动成且无方向"（不学障碍）→ BFS 始终认为 A 可达 → 只走卡住分支。
  auto scene = {Res(7, 5, "A"), Res(3, 5, "B")};
  for (int i = 0; i < 2; ++i) {
    Perception f = Frame(10 + i, "s", 5, 5, scene);
    SetReceipt(f, /*moved=*/false);  // 无方向：不学障碍、只计卡住
    (void)s.decide(f, nullptr);
  }
  Perception f12 = Frame(12, "s", 5, 5, scene);
  SetReceipt(f12, /*moved=*/false);
  ActionChoice c = s.decide(f12, nullptr);  // stuck 已到 3 → 换目标
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "left", c);  // 排除 A 后必选 B；重选 A 会朝 right
}

// 10/31 R6-A 回归：无已知资源不再 wait，改为前沿探索（密障碍图 8/40 分的病根）。
// 试验台：中心 (9,9)、18×18、无实体 → 已见 9×9、四周有未见图 → 必须动。
void Test17_ExploreWhenNoResource() {
  std::printf("[TEST] 17 无已知资源 → 探索未见图（不原地 wait）\n");
  CollectStrategy s;
  Perception f = Frame(10, "s", 9, 9, {});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up" || c.direction == "down" ||
          c.direction == "left" || c.direction == "right", c);
}

// 10/31 R6-A：全图已见（1×1）时没有前沿，不能瞎走 → wait。
void Test18_ExploreNothingLeftWaits() {
  std::printf("[TEST] 18 全图已见 → 无可探索区域时 wait\n");
  CollectStrategy s;
  Perception f = Frame(10, "s", 0, 0, {});
  f.map_width = 1;
  f.map_height = 1;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "wait", c);
}

// 10/31 R6-A：已见区域只占地图左半（9×9 图、self 在左中），未见图全在右侧，
// 探索必须朝右推进；若按"最近前沿"也会朝右，但落点选择必须避坑：
// 这里钉的是"朝最大新见区域方向走"的端到端行为。
void Test19_ExploreTowardUnseenSide() {
  std::printf("[TEST] 19 前沿探索朝未见图方向 → right\n");
  CollectStrategy s;
  Perception f = Frame(10, "s", 0, 4, {});
  f.map_width = 9;
  f.map_height = 9;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move" && c.direction == "right", c);
}

// 10/31 V8.6：阶段二 E_BLOCKED 可能是敌车占位/幻术反转——视野内明明是空地
// 的格子不得学成永久的墙（旧实现会把 14–20 次动态阻挡全部记成障碍）。
void Test20_DynamicBlockNotLearned() {
  std::printf("[TEST] 20 阶段二动态阻挡（视野内空地）→ 不学成墙\n");
  CollectStrategy s;
  s.set_dynamic_blockers(true);
  Perception f1 = Frame(10, "s", 5, 5, {Res(8, 5, "A")});
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s", 5, 5, {Res(8, 5, "A")});
  SetReceipt(f2, /*moved=*/false, /*dir=*/"right", /*error=*/"E_BLOCKED");
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "move" && c.direction == "right", c);
}

// 10/31 V8.6：已见格覆盖率接口（战斗层判断探索进度用）。
void Test21_ExploredFraction() {
  std::printf("[TEST] 21 已见覆盖率 → 单帧 9×9 = 25%%（18×18）\n");
  CollectStrategy s;
  Perception f = Frame(10, "s", 4, 4, {});
  (void)s.decide(f, nullptr);
  const double cov = s.explored_fraction(18, 18);
  CHECK(cov > 0.24 && cov < 0.26);
}

// 10/31 V8.6：探索开关关闭（阶段二资源饱和后）→ 无资源回 wait 交回战斗层。
void Test22_ExploreDisabledWaits() {
  std::printf("[TEST] 22 探索关闭 + 无资源 → wait\n");
  CollectStrategy s;
  s.set_explore_enabled(false);
  Perception f = Frame(10, "s", 5, 5, {});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "wait", c);
}

// 10/31 V8.6：视野内资源消失（被对手抢走）→ 立即删记忆，不再白跑空趟。
void Test23_GhostResourceInViewDropped() {
  std::printf("[TEST] 23 视野内资源被抢 → 删记忆，不再空跑\n");
  CollectStrategy s;
  s.set_explore_enabled(false);
  Perception f1 = Frame(10, "s", 5, 5, {Res(7, 5, "A")});
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s", 5, 5, {});
  f2.score.resource_value = 10;  // 分数上涨但不在 A 格上 = 对手采走
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "wait", c);
}

// 10/31 V8.6：对手更近的资源不追（阶段二），改去自己能赢的那个。
void Test24_EnemyCloserResourceSkipped() {
  std::printf("[TEST] 24 对手更近的资源跳过 → 改去安全资源\n");
  CollectStrategy s;
  s.set_dynamic_blockers(true);
  Entity enemy = RobotEntity(6, 5, "B");
  Perception f = Frame(10, "s", 5, 5,
                       {Res(7, 5, "contested", 10),
                        Res(5, 9, "safe", 10), enemy});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("contested") == std::string::npos, c);
  CHECK_C(c.direction == "down", c);
}

// V8.7：高价值任务未完成 → 高价值资源可多绕 10 步，不再被近处普通资源截胡。
void Test25_HighTaskPrefersHighResource() {
  std::printf("[TEST] 25 高价值任务未完成 → 优先远处高价值资源\n");
  CollectStrategy s;
  Perception f =
      Frame(10, "s", 5, 5, {Res(5, 3, "near_normal", 10),
                            Res(9, 5, "far_high", 20)});
  f.missions.push_back(ResourceTask("collect_high", "high_resources"));
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move" && c.direction == "right", c);
  CHECK_C(c.reason.find("far_high") != std::string::npos, c);
  CHECK_C(c.reason.find("高价值任务优先") != std::string::npos, c);
}

// 已完成的高价值任务不能继续扭曲目标选择（裁判会一直保留 missions 列表）。
void Test26_CompletedHighTaskDoesNotDistort() {
  std::printf("[TEST] 26 高价值任务已完成 → 恢复最近普通资源\n");
  CollectStrategy s;
  Perception f =
      Frame(10, "s", 5, 5, {Res(5, 3, "near_normal", 10),
                            Res(9, 5, "far_high", 20)});
  f.missions.push_back(ResourceTask("collect_high", "high_resources"));
  f.score.mission_completed_ids.push_back("collect_high");
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move" && c.direction == "up", c);
  CHECK_C(c.reason.find("near_normal") != std::string::npos, c);
}

// 普通任务未完成时反过来优先普通资源：高价值资源只近 2 步也不抢。
void Test27_NormalTaskPrefersNormalResource() {
  std::printf("[TEST] 27 普通任务未完成 → 优先普通资源而非更近高价值\n");
  CollectStrategy s;
  Perception f =
      Frame(10, "s", 5, 5, {Res(5, 9, "normal", 10),
                            Res(7, 5, "high", 20)});
  f.missions.push_back(ResourceTask("collect_normal", "normal_resources"));
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move" && c.direction == "down", c);
  CHECK_C(c.reason.find("普通资源任务优先") != std::string::npos, c);
}

// V8.7 高价值搜索：没有高价值记忆时，13 步外的普通资源先不追，转去探图。
void Test28_HighSearchExploresInsteadOfFarNormal() {
  std::printf("[TEST] 28 高价值任务未完成 → 不追远处普通资源，先探图\n");
  CollectStrategy s;
  Perception f = Frame(10, "s", 5, 5, {Res(5, 15, "far_normal", 10)});
  f.missions.push_back(ResourceTask("collect_high", "high_resources"));
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("far_normal") == std::string::npos, c);
  CHECK_C(c.reason.find("探索") != std::string::npos, c);
}

// 搜索模式不能连近处普通资源也放弃：4 步内照拿，保持资源线不断。
void Test29_HighSearchStillTakesNearNormal() {
  std::printf("[TEST] 29 高价值搜索中 → 近处普通资源照拿\n");
  CollectStrategy s;
  Perception f = Frame(10, "s", 5, 5, {Res(5, 9, "near_normal", 10)});
  f.missions.push_back(ResourceTask("collect_high", "high_resources"));
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move" && c.direction == "down", c);
  CHECK_C(c.reason.find("near_normal") != std::string::npos, c);
}

// V8.8 开局冲中心：高价值未找到时，普通资源要沿"更靠近中心"的方向选，
// 不能被出生点旁边的小资源反复截胡；同时仍然捡普通资源，不是裸冲中心。
void Test30_HighSearchPrefersTowardCenterNormal() {
  std::printf("[TEST] 30 高价值搜索 → 普通资源向中心倾斜\n");
  CollectStrategy s;
  Perception f =
      Frame(10, "s", 1, 1, {Res(2, 1, "near_behind", 10),
                            Res(1, 6, "toward_center", 10)});
  f.missions.push_back(ResourceTask("collect_high", "high_resources"));
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "down", c);
  CHECK_C(c.reason.find("toward_center") != std::string::npos, c);
  CHECK_C(c.reason.find("中心") != std::string::npos, c);
}

}  // namespace

int main() {
  std::printf("==== CollectStrategy 离线自检（30 组场景）====\n");
  Test01_EmptyPerception();
  Test02_NearestStraight();
  Test03_MemoryOutOfSight();
  Test04_ValueDoesNotDisturb();
  Test05_ObstacleDetour();
  Test06_BlockedLearnsObstacle();
  Test07_StuckSwitchTarget();
  Test08_CollectedRemoved();
  Test09_NewSessionClears();
  Test10_Idempotent();
  Test11_Stage1Whitelist();
  Test12_DeadAndRespawn();
  Test13_ScoreUpKeepsOutOfSight();
  Test14_StandingOnTargetWaits();
  Test15_IllegalDirectionNotLearned();
  Test16_StuckRepicksDifferentTarget();
  Test17_ExploreWhenNoResource();
  Test18_ExploreNothingLeftWaits();
  Test19_ExploreTowardUnseenSide();
  Test20_DynamicBlockNotLearned();
  Test21_ExploredFraction();
  Test22_ExploreDisabledWaits();
  Test23_GhostResourceInViewDropped();
  Test24_EnemyCloserResourceSkipped();
  Test25_HighTaskPrefersHighResource();
  Test26_CompletedHighTaskDoesNotDistort();
  Test27_NormalTaskPrefersNormalResource();
  Test28_HighSearchExploresInsteadOfFarNormal();
  Test29_HighSearchStillTakesNearNormal();
  Test30_HighSearchPrefersTowardCenterNormal();

  if (g_fail == 0) {
    std::printf("==== 全部通过 ====\n");
    return 0;
  }
  std::printf("==== 失败 %d 项 ====\n", g_fail);
  std::printf("（断言级 %d 项失败；场景清单见 docs/collect_design.md §6.5）\n",
              g_fail);
  return 1;
}
