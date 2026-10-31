// combat_selftest.cpp —— CombatStrategy 规则层离线自检（87 组场景，不依赖 ROS/裁判/LLM）
//
// 用法：ros2 run rmtc_bot combat_selftest
// 场景清单：docs/combat_design.md §5.5（1–13 组）+ 10/28 v2 优化（14–21 组）+
// 10/28 v2.1 死亡修复 P1–P3（22–24 组，docs/analysis/death_mode_analysis）+
// 10/28 自对弈 round1 R1 生存门（25 组，docs/analysis/selfplay_round1_20261028）+
// 10/28 自对弈 round2 R2-A 对称换血收紧 / R2-B 回血路线（26–32 组，
// docs/analysis/selfplay_round2_20261028）+ 10/28 R3-A 主动换血（37–39 组）+
// 10/29 V4 任务销账（40 组，docs/analysis/复盘_20261029-205800.md：裁判全程
// 保留 missions 列表，已完成任务点不得再回头）+ 10/29 V5 环形巡逻（41–42 组，
// 用户拍板"金边银肚皮草角"：距边 D 格圆角方形环，四角切角避出生/复活点）+
// 10/29 V6 低血不逃命/脱环不回吸（43–46 组，用户实测 500ms 场：左上徘徊、
// 低血空转）+ 10/30 V7 集火参与 + 参战信念门 + 防御目标落井下石（47–56 组，
// 用户拍板"人多聚堆就上前劝架、任务/资源分作为参战信念、防御目标照打"）+
// 10/30 V8 R5-A~F（47b/48c/57–67 组，docs/analysis/v8_engage_illusion_paid_
// 20261030.md：V7 首场练习赛实测——资源饱和线不可达致 0 集火、Illusion 漂移
// 贴边、WorldSplit 鬼墙绕圈徘徊、巡逻锚定劫持、残血回本敢死队、参战锁存）+
// 10/30 V8 R5-G（68–69 组，V8 练习赛实测：顺序满圈被多敌避让劫持到 tick 470+、
// 踩格覆盖 tick 317 已满 → 实证改覆盖率门槛 90%）+ 10/30 V8.1 R5-H/I（70–72 组，
// 用户反馈：血量碾压仍避战 26/27 次；巡逻 BFS 横穿地图中间 + 复活帧被跳过）+
// 10/30 V8.2 R5-J~N（73–79 组，用户反馈：血量高就追、环改直角正方形、删多敌
// 避让、交火范围内不白挨打、超过 500 分照常打——饱和不再当"不追/不还手"
// 的理由）+ 10/31 V8.4 R5-P/Q（80–85 组，追击纪律/封角绞杀）+
// 10/31 V8.5 R6-B（86 组，v90 实测贴边幻觉补偿空转 7 Tick：E_OOB 回执即停补偿）+
// 10/31 V8.5 R6-C（87 组，环上单格障碍绕行锁存，防 A↔B 两格死循环）。
// 注：组号 34 空缺（历史上未启用），测试函数实为 88 个。
//
// 断言全部按"协议事实 + 设计稿规则层"设计，不锁策略内部实现（记忆地图复用形态、
// LLM 意图怎么接都不影响本测试台）：
//   - 直线唯一解断言具体方向（场景 4：唯一射程内敌车 → attack right）
//   - 约束类断言用宽松形式（场景 5–8 只断"不 attack 某目标/方向"，
//     换目标或 wait 都算过，避免实现细节不同导致误报）
//   - 安全网场景（1/2/9/10/11/12）占位实现也应为绿，防止实现时倒退
//   - 游走兜底（13，10/21 线上坑②回归）：占位 wait 下红，防止"任务完成后
//     原地裸 wait 浪费整图剩余资源"回归（设计稿 §2）
//   - 14–21 为 10/28 v2 优化场景：可击杀优先/断线撤离/防御保命/
//     障碍避让/资源饱和任务优先/游走轮换/被围防御（15 的"对抗分饱和门"已在
//     R5-N 反转为"饱和照打"）
//
// 注意：CombatStrategy 尚未实现时（10/27 开工前）仅核心场景（4）与
// 游走兜底（13）红、其余 11 组（安全网/约束类断言）wait 即绿——这是预期的
// TDD 基线；规则层实现正确后应全部转绿。

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "rmtc_bot/combat_strategy.hpp"

using rmtc_bot::ActionChoice;
using rmtc_bot::CombatStrategy;
using rmtc_bot::Entity;
using rmtc_bot::LlmResult;
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

Entity RobotE(int x, int y, const std::string &team, bool alive = true,
              bool defending = false, bool protected_ = false, int hp = 100) {
  Entity e;
  e.type = "robot";
  e.team = team;
  e.has_absolute = true;
  e.abs_x = x;
  e.abs_y = y;
  e.has_hp = true;
  e.hp = hp;
  e.defending = defending;
  e.protected_flag = protected_;
  // 注意：alive 无独立字段，用 hp=0 表示阵亡车（协议实体含 hp）
  (void)alive;
  if (!alive) e.hp = 0;
  return e;
}

// 基础帧：18×18 地图、本车 (5,5) 存活、给定实体与 tick/session
// vision_radius=4（与裁判一致）：R5-C 障碍遗忘 / 信念资源消失归因都按
// "视野内"判定，测试台必须给真实半径，否则这些逻辑在测试里永远不触发。
Perception Frame(int tick, const std::string &session, int x, int y,
                 const std::vector<Entity> &entities = {}) {
  Perception p;
  p.tick = tick;
  p.session_id = session;
  p.map_width = 18;
  p.map_height = 18;
  p.vision_radius = 4;
  p.self.has_position = true;
  p.self.x = x;
  p.self.y = y;
  p.self.has_alive = true;
  p.self.alive = true;
  p.self.has_hp = true;
  p.self.hp = 100;
  p.entities = entities;
  return p;
}

void SetSelfProtected(Perception &p, bool v) { p.self.protected_flag = v; }

// R5-G 测试助手（10/30 V8 练习赛实测）：沿 18×18 环（d=5）顺序"踩"一遍环格，
// 让巡逻覆盖集积累（与 BuildPatrolRing 同构硬编码，V8.2 R5-K 直角矩形环，
// 上→右→下→左顺时针，共 28 格）。cells<0 → 走满全环；否则只走前 cells 格
// （测覆盖率门槛）。返回后覆盖率为 cells/28（或 100%）。
void WalkRing(CombatStrategy &s, int &tick, int cells = -1) {
  static const std::vector<std::pair<int, int>> ring = {
      {5, 5},  {6, 5},  {7, 5},  {8, 5},  {9, 5},  {10, 5}, {11, 5},
      {12, 5}, {12, 6}, {12, 7}, {12, 8}, {12, 9}, {12, 10}, {12, 11},
      {12, 12}, {11, 12}, {10, 12}, {9, 12}, {8, 12}, {7, 12}, {6, 12},
      {5, 12}, {5, 11}, {5, 10}, {5, 9}, {5, 8}, {5, 7}, {5, 6}};
  const int n = (cells < 0) ? static_cast<int>(ring.size()) : cells;
  for (int i = 0; i < n; ++i) {
    Perception f = Frame(tick++, "s", ring[static_cast<size_t>(i)].first,
                         ring[static_cast<size_t>(i)].second);
    (void)s.decide(f, nullptr);
  }
}

// 障碍实体（绝对坐标直填）
Entity ObstacleE(int x, int y) {
  Entity e;
  e.type = "obstacle";
  e.has_absolute = true;
  e.abs_x = x;
  e.abs_y = y;
  return e;
}

// 资源实体（绝对坐标 + 价值 + id）
Entity ResourceE(int x, int y, int value, const std::string &id) {
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

// reach 类任务（带坐标）
rmtc_bot::Mission ReachMission(const std::string &id, int x, int y) {
  rmtc_bot::Mission m;
  m.id = id;
  m.type = "reach";
  m.has_position = true;
  m.pos_x = x;
  m.pos_y = y;
  return m;
}

// effective_damage 类任务（战斗任务，无坐标——参战信念门的开锁钥匙）
rmtc_bot::Mission DamageMission(const std::string &id) {
  rmtc_bot::Mission m;
  m.id = id;
  m.type = "effective_damage";
  return m;
}

// ---- 13 组场景 ----

void Test01_EmptyPerception() {
  std::printf("[TEST] 1 空感知不崩溃 → wait\n");
  CombatStrategy s;
  Perception p;  // 全默认：tick=-1、无位置、无地图
  ActionChoice c = s.decide(p, nullptr);  // llm=nullptr 也必须安全
  CHECK_C(c.action == "wait", c);
}

void Test02_DeadWaits() {
  std::printf("[TEST] 2 阵亡 → wait\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(7, 5, "B")});
  f.self.alive = false;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "wait", c);
}

void Test03_SelfProtectedNoAttack() {
  std::printf("[TEST] 3 我方保护期 → 不 attack\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(7, 5, "B")});  // 敌车直线 2 格内
  SetSelfProtected(f, true);  // 我方出生保护期：协议禁止攻击
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action != "attack", c);
}

void Test04_AttackInRange() {
  std::printf("[TEST] 4 直线 2 格内敌车（我方领先一枪）→ attack right\n");
  CombatStrategy s;
  // R2-A：100 vs 100 不再对称换血；hp 75 满足"严格领先一枪"（100 ≥ 75+20）
  Perception f =
      Frame(10, "s", 5, 5, {RobotE(7, 5, "B", true, false, false, 75)});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);  // 唯一射程内目标，方向唯一解
}

void Test05_OutOfRangeNoAttack() {
  std::printf("[TEST] 5 直线 3 格（射程外）→ 不 attack\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(8, 5, "B")});  // 距 3，超出 1–2 射程
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action != "attack", c);  // 射程外空击 = 浪费 Tick
}

void Test06_DiagonalNoAttack() {
  std::printf("[TEST] 6 斜线敌车 → 不朝斜向 attack\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(6, 6, "B")});  // 对角，非直线
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action != "attack", c);  // 协议：攻击只沿 up/down/left/right 直线
}

void Test07_DefendingTargetNowAttacked() {
  std::printf("[TEST] 7 目标防御中（V7/R4-A 落井下石）→ 照打不误\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(7, 5, "B", true, /*defending=*/true)});
  ActionChoice c = s.decide(f, nullptr);
  // 10/30 V7 用户拍板：防御中目标照打——半伤落井下石，且其本 Tick 不反击
  // （k_in 已排除），白嫖伤害；100 vs 100 同血即打（R3-A）→ attack right
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);

  // 半伤折算可击杀线：hp=12 的防御目标一枪打不死（10 伤）→ 第二遍换血判定
  // 放行（12 < 40 残血条款）→ 仍 attack；hp=9 防御目标一枪收头 → attack
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(7, 5, "B", true, true, false, 12)});
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "attack", c2);
  Perception f3 = Frame(12, "s", 5, 5,
                        {RobotE(7, 5, "B", true, true, false, 9)});
  ActionChoice c3 = s.decide(f3, nullptr);
  CHECK_C(c3.action == "attack", c3);
}

void Test08_ProtectedTargetNoAttack() {
  std::printf("[TEST] 8 目标保护期 → 不 attack 它\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(7, 5, "B", true, false, /*protected=*/true)});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(!(c.action == "attack" && c.direction == "right"), c);  // 打了白打（保护 0）
}

void Test09_LegalActionSet() {
  std::printf("[TEST] 9 动作与方向合法性安全网\n");
  CombatStrategy s;
  for (int i = 0; i < 3; ++i) {
    Perception f = Frame(10 + i, "s", 5, 5,
                         {RobotE(7, 5, "B"), RobotE(5, 3, "C", false)});
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move" || c.action == "attack" || c.action == "defend" ||
            c.action == "wait", c);
    if (c.action == "move" || c.action == "attack") {
      CHECK_C(c.direction == "up" || c.direction == "down" ||
              c.direction == "left" || c.direction == "right", c);
    }
  }
}

void Test10_Idempotent() {
  std::printf("[TEST] 10 幂等 → 同帧两次 decide 完全一致\n");
  CombatStrategy s;
  Perception p = Frame(10, "s", 5, 5, {RobotE(7, 5, "B")});
  ActionChoice c1 = s.decide(p, nullptr);
  ActionChoice c2 = s.decide(p, nullptr);
  CHECK_CC(c1.action == c2.action, c1, c2);
  CHECK_CC(c1.direction == c2.direction, c1, c2);
  CHECK_CC(c1.reason == c2.reason, c1, c2);  // 恢复重发同 Tick 时输出逐字节一致
}

void Test11_LlmNullSafe() {
  std::printf("[TEST] 11 LLM 结论为空 → 规则层仍合法决策\n");
  CombatStrategy s;
  Perception p = Frame(10, "s", 5, 5, {RobotE(7, 5, "B")});
  ActionChoice c = s.decide(p, nullptr);  // llm 永远可能为 nullptr
  CHECK_C(c.action == "move" || c.action == "attack" || c.action == "defend" ||
          c.action == "wait", c);
}

void Test12_NewSessionClears() {
  std::printf("[TEST] 12 换场清空 → 无旧目标残留\n");
  CombatStrategy s;
  Perception f1 = Frame(10, "s1", 5, 5, {RobotE(7, 5, "B")});
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s1", 6, 5, {RobotE(7, 5, "B")});
  (void)s.decide(f2, nullptr);  // 规则层已有敌车/目标状态

  s.on_new_session("s2");
  Perception p3 = Frame(1, "s2", 5, 5);  // 新场次：无任何实体
  ActionChoice c = s.decide(p3, nullptr);
  // 状态已清：旧敌车记忆若残留会朝记忆方向 attack；游走兜底（场景 13）下
  // 空帧 move 合法，故只断"不 attack"，不锁具体动作
  CHECK_C(c.action != "attack", c);
}

void Test13_IdleRoams() {
  std::printf("[TEST] 13 无目标空闲 → 不裸 wait（朝未探索区/中心游走）\n");
  CombatStrategy s;
  bool moved = false;
  for (int t = 10; t <= 15; ++t) {
    Perception f = Frame(t, "s", 5, 5);  // 连续无实体帧：无目标、无任务、无威胁
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move" || c.action == "attack" || c.action == "defend" ||
            c.action == "wait", c);
    if (c.action == "move") {
      CHECK_C(c.direction == "up" || c.direction == "down" ||
              c.direction == "left" || c.direction == "right", c);
      moved = true;
    }
  }
  // 线上坑②（10/21 stage2 实测）：任务完成后原地裸 wait 到结束，剩 140 价值
  // 资源无人捡。允许实现先观察 1–2 帧，但连续空闲帧中必须出现主动游走
  CHECK(moved);
}

// ---- 14–21 组：10/28 v2 决策层接入与战术优化 ----

void Test14_KillableFirst() {
  std::printf("[TEST] 14 可击杀优先 → 舍近处非击杀目标，先收人头\n");
  CombatStrategy s;
  // 右(6,5) hp=25 非击杀（打一枪剩 5）；上(5,4) hp=15 一枪收头
  Perception f = Frame(10, "s", 5, 5, {RobotE(6, 5, "B", true, false, false, 25),
                                       RobotE(5, 4, "C", true, false, false, 15)});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "up", c);  // 收 C 的人头（击杀奖励+对手扣 D）
}

void Test15_SaturationStillFights() {
  std::printf("[TEST] 15 对抗分饱和后照常换血（R5-N）→ 非击杀目标也打\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(6, 5, "B", true, false, false, 90)});
  f.score.effective_damage = 500;  // 对抗分满格：击杀仍扣对手 60 D、掐资源
  ActionChoice c = s.decide(f, nullptr);
  // R5-N：饱和不再挡进攻——同血/领先换血照打（旧行为"饱和只收头"= 白挨打）
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);

  Perception f2 = Frame(11, "s", 5, 5, {RobotE(6, 5, "B", true, false, false, 10)});
  f2.score.effective_damage = 500;  // 可击杀：更优先
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "attack", c2);
  CHECK_C(c2.direction == "right", c2);
}

void Test16_RetreatBreaksLine() {
  std::printf("[TEST] 16 低血撤离断线 → 垂直脱离直线射程（不沿直线逃）\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(6, 5, "B")});  // 直线 1 格，正对枪口
  f.self.hp = 30;
  ActionChoice c = s.decide(f, nullptr);
  // 沿直线左逃仍在射程（2 格内），垂直 up/down 才算脱离
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up" || c.direction == "down", c);
}

void Test17_DefendUnderFocus() {
  std::printf("[TEST] 17 被集火不防御必死 → defend（防御减半可活）\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 5, "B"), RobotE(5, 6, "C")});
  f.self.hp = 35;  // 两路直线火力 40 伤：不防御死，防御 20 伤可活
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "defend", c);
}

void Test18_RetreatAvoidsObstacle() {
  std::printf("[TEST] 18 低血撤离避障 → 障碍方向不可选，走开阔方向\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 5, "B"), ObstacleE(5, 4)});  // 上方被墙堵死
  f.self.hp = 30;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "down", c);  // 唯一零威胁合法格
}

void Test19_MissionFirstWhenSaturated() {
  std::printf("[TEST] 19 资源饱和任务优先 / 未饱和采集优先\n");
  CombatStrategy s;
  const std::vector<Entity> ents = {ResourceE(5, 9, 10, "r1")};
  Perception f = Frame(10, "s", 5, 5, ents);
  f.score.resource_value = 0;  // 未饱和：先采集（资源在正下方 → down）
  f.missions = {ReachMission("m1", 9, 5)};
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "down", c);

  Perception f2 = Frame(11, "s", 5, 5, ents);
  f2.score.resource_value = 120;  // 资源分已满格：任务优先（任务点在正右 → right）
  f2.missions = f.missions;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "right", c2);
}

void Test20_WanderRotatesWhenBlocked() {
  std::printf("[TEST] 20 游走被挡轮换 → 不重复撞同一方向\n");
  CombatStrategy s;
  // V8.6：资源未饱和时采集层会去探索，测巡逻轮换须先让资源侧饱和
  Perception f = Frame(10, "s", 5, 5);  // 无实体 → 游走
  f.score.resource_value = 120;
  ActionChoice c1 = s.decide(f, nullptr);
  CHECK_C(c1.action == "move", c1);
  const std::string d1 = c1.direction;

  Perception f2 = Frame(11, "s", 5, 5);  // 上一步 move 被挡（回执）
  f2.score.resource_value = 120;
  f2.self.has_last_action = true;
  f2.self.last_tick = 10;
  f2.self.last_action = "move";
  f2.self.last_moved = false;
  f2.self.has_last_direction = true;
  f2.self.last_effective_direction = d1;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction != d1, c2);  // 换方向，不撞墙
}

void Test21_LowHpTrappedDefends() {
  std::printf("[TEST] 21 低血被围无路可撤 → defend（比裸挨打强）\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 5, "B"), ObstacleE(5, 4), ObstacleE(5, 6),
                        ObstacleE(4, 5)});  // 三面墙 + 敌车占右
  f.self.hp = 15;  // V8.6：35 血吃 20 伤不死，按资源优先不再防御；15 血才是必死
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "defend", c);
}

// ---- 22–24 组：10/28 v2.1 死亡修复 P1–P3 回归 ----

void Test22_RetreatNoOscillation() {
  std::printf("[TEST] 22 低血避火不振荡（V6）→ 枪线上脱离，脱线后步进不再入线\n");
  CombatStrategy s;
  // 追兵 B 在 3 格外同排（InGunLine ≤4 → 视为枪线上），任务点在追兵身后
  // (11,5)。V6：枪线上先垂直脱离；脱线后任务/巡逻步进有安全门，任何一步
  // 都不得重新落入 B 的枪线（旧逻辑"落任务推进→被撤出→又走回"振荡复发）。
  int x = 5, y = 5;
  for (int t = 10; t < 18; ++t) {
    Perception f = Frame(t, "s", x, y,
                         {RobotE(8, 5, "B", true, false, false, 100)});
    f.self.hp = 30;
    f.missions = {ReachMission("m1", 11, 5)};  // 追兵身后：不能朝它推进
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    const int nx =
        x + (c.direction == "left" ? -1 : c.direction == "right" ? 1 : 0);
    const int ny =
        y + (c.direction == "up" ? -1 : c.direction == "down" ? 1 : 0);
    const bool on_line =
        (ny == 5 && std::abs(nx - 8) >= 1 && std::abs(nx - 8) <= 4);
    CHECK(!on_line);  // 任何一步都不落回 B 的枪线（防 1 格振荡）
    if (t == 10) {
      CHECK(c.direction == "up" || c.direction == "down");  // 首帧垂直脱线
    }
    x = nx;
    y = ny;
  }
}

void Test23_LowHpNoFleeOffLine() {
  std::printf("[TEST] 23 低血非枪线不逃命（V6）→ 照常推进，但绝不走进枪线\n");
  CombatStrategy s;
  // self [8,0] hp=10，C 车 [9,2]（Chebyshev 2 斜对角，不在枪线），任务 (9,9)。
  // V5 会进撤离态 left 逃命；V6：无真实威胁不逃，任务照走（走 x=8 列绕开
  // C 的同列枪线），步进安全门保证任何一步都不落入 C 的枪线。
  int x = 8, y = 0;
  for (int t = 10; t < 16; ++t) {
    Perception f = Frame(t, "s", x, y,
                         {RobotE(9, 2, "C", true, false, false, 100)});
    f.self.hp = 10;
    f.missions = {ReachMission("m1", 9, 9)};
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    const int nx =
        x + (c.direction == "left" ? -1 : c.direction == "right" ? 1 : 0);
    const int ny =
        y + (c.direction == "up" ? -1 : c.direction == "down" ? 1 : 0);
    const bool on_line =
        (ny == 2 && std::abs(nx - 9) >= 1 && std::abs(nx - 9) <= 4) ||
        (nx == 9 && std::abs(ny - 2) >= 1 && std::abs(ny - 2) <= 4);
    CHECK(!on_line);  // 不走进 C 的枪线
    x = nx;
    y = ny;
  }
}

void Test24_LowHpKeepsKillOnly() {
  std::printf("[TEST] 24 低血收头不赔命（R1 生存门）→ 同归于尽的头不收，防御兜底\n");
  CombatStrategy s;
  // 20 vs 16：一枪收头，但同 Tick 被反打 20 必死（同归于尽）→ 不再 attack，
  // 防御减半（10 < 20）可活 → defend
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(7, 5, "B", true, false, false, 16)});
  f.self.hp = 20;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "defend", c);

  // 非击杀目标：hp=20 vs HP30（一枪打不死）→ 防御兜底，不换血
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(7, 5, "B", true, false, false, 30)});
  f2.self.hp = 20;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "defend", c2);
}

// ---- 25 组：10/28 自对弈 round1（镜像同归于尽）R1 生存门回归 ----

void Test25_SurvivableKillTaken() {
  std::printf("[TEST] 25 可活收头仍收（R1 生存门）→ 反打打不死就照收\n");
  CombatStrategy s;
  // 40 vs 20：收头后挨 20 剩 20 血，生存门放行 → attack
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(7, 5, "B", true, false, false, 20)});
  f.self.hp = 40;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);
}

// ---- 26–32 组：10/28 自对弈 round2（对称换血 + 撤离死锁）R2-A/R2-B 回归 ----

// 构造 LLM 成功结论（模拟 llm_brain 的 latest_result；content 为 LLM 返回 JSON）
rmtc_bot::LlmResult LlmOk(const std::string &json) {
  rmtc_bot::LlmResult r;
  r.status = rmtc_bot::LlmResult::Status::SUCCESS;
  r.content = json;
  return r;
}

void Test26_IsolatedTradeKept() {
  std::printf("[TEST] 26 孤立目标换血保留（R2 终版）→ 100 vs 100 attack\n");
  CombatStrategy s;
  // R2 期间攻击倾向门三个版本在官方陪练回归中全部掉伤害掉排名 → 撤销；
  // 镜像 20/20 死锁由 R1 生存门（20 血互杀被拦）+ R2-B 回血路线兜底
  Perception f = Frame(10, "s", 5, 5, {RobotE(7, 5, "B")});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);
  // 20 vs 20：可击杀（20 ≤ 20）但生存门 k_in×20=20 ≥ 20 → 不收头（R1）
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(7, 5, "B", true, false, false, 20)});
  f2.self.hp = 20;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action != "attack", c2);
}

void Test27_AheadByShotAttacks() {
  std::printf("[TEST] 27 领先一枪仍打（R2）→ 100 vs 80 attack\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(7, 5, "B", true, false, false, 80)});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);
}

void Test28_RetreatHealRoute() {
  std::printf("[TEST] 28 撤离态回血路线（R2-B）→ 20 血朝 LLM 回血点走\n");
  CombatStrategy s;
  // 敌车 3 格外可见（Chebyshev ≤4 → 撤离态）；LLM 结论带回血点 [14,9]
  Perception f = Frame(10, "s", 5, 5, {RobotE(8, 5, "B")});
  f.self.hp = 20;
  LlmResult r = LlmOk(
      "{\"goals\":[\"reach\"],"
      "\"event_plan\":{\"points\":[[14,9]],\"avoid\":false,\"until_tick\":9999,"
      "\"reason\":\"回血点\"},\"summary\":\"s\"}");
  ActionChoice c = s.decide(f, &r);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("回血") != std::string::npos, c);
}

void Test29_NoHealWhenHealthy() {
  std::printf("[TEST] 29 满血不走回血路线（R2-B）→ 不朝回血点跑\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(8, 5, "B")});
  LlmResult r = LlmOk(
      "{\"goals\":[\"reach\"],"
      "\"event_plan\":{\"points\":[[14,9]],\"avoid\":false,\"until_tick\":9999,"
      "\"reason\":\"回血点\"},\"summary\":\"s\"}");
  ActionChoice c = s.decide(f, &r);
  CHECK_C(c.reason.find("回血") == std::string::npos, c);
}

void Test30_ExpiredPlanNoHeal() {
  std::printf("[TEST] 30 过期结论不回血（R2-B）→ until_tick 过期作废\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(8, 5, "B")});
  f.self.hp = 20;
  LlmResult r = LlmOk(
      "{\"goals\":[\"reach\"],"
      "\"event_plan\":{\"points\":[[14,9]],\"avoid\":false,\"until_tick\":5,"
      "\"reason\":\"回血点\"},\"summary\":\"s\"}");
  ActionChoice c = s.decide(f, &r);
  CHECK_C(c.reason.find("回血") == std::string::npos, c);
}

void Test31_HealWhenNoEnemyVisible() {
  std::printf("[TEST] 31 低血无敌可见（R2-B）→ 50 血也朝回血点走\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5);  // 无实体：无威胁
  f.self.hp = 50;  // < 60（heal_wanted），≥ 40（不进撤离态）
  LlmResult r = LlmOk(
      "{\"goals\":[\"reach\"],"
      "\"event_plan\":{\"points\":[[14,9]],\"avoid\":false,\"until_tick\":9999,"
      "\"reason\":\"回血点\"},\"summary\":\"s\"}");
  ActionChoice c = s.decide(f, &r);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("回血") != std::string::npos, c);
}

void Test32_BadLlmContentNoCrash() {
  std::printf("[TEST] 32 LLM 返回非 JSON（R2-B）→ 沿用旧结论，合法决策不崩\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(8, 5, "B")});
  f.self.hp = 20;
  LlmResult r = LlmOk("不是 JSON");
  ActionChoice c = s.decide(f, &r);
  CHECK_C(c.action == "move" || c.action == "attack" || c.action == "defend" ||
          c.action == "wait", c);
}

void Test33_AheadByShotEvenAfterRetaliation() {
  std::printf("[TEST] 33 对方还过手但我领先一枪（R2）→ 仍打\n");
  CombatStrategy s;
  // t10 首遇换一枪；t11 我方 100→80、对方 100→60（第三方打的），
  // 80 ≥ 60+20 → 领先一枪条款放行
  Perception f = Frame(10, "s", 5, 5, {RobotE(7, 5, "B")});
  ActionChoice c0 = s.decide(f, nullptr);
  CHECK_C(c0.action == "attack", c0);
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(7, 5, "B", true, false, false, 60)});
  f2.self.hp = 80;
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);
}

// ---- 35–36 组：10/28 官方陪练 20:44 批次六连死（R2-C 多敌聚集避让） ----

void Test35_NoClusterFleeWhenMissionPending() {
  std::printf("[TEST] 35 任务未做完多敌不再逃跑（V8.2 R5-L）→ 照常巡逻\n");
  CombatStrategy s;
  // 未完成的无坐标任务（normal_resources）→ 参战开关关；B/C 两车斜对角可见、
  // 血量均满（无血量便宜）→ R5-L 删除 R2-C 避让：不逃、不集火，游走兜底巡逻
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 6, "B"), RobotE(6, 4, "C")});
  rmtc_bot::Mission m;
  m.id = "c1";
  m.type = "normal_resources";
  m.has_target = true;
  m.target = 2;
  f.missions = {m};
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
  CHECK_C(c.reason.find("多敌聚集") == std::string::npos, c);
}

void Test36_SingleEnemyNoClusterAvoid() {
  std::printf("[TEST] 36 单敌可见不触发避让（R2-C）→ 保持原游走\n");
  CombatStrategy s;
  // 斜对角单敌（不在枪线，攻击判定不打；视野内仅 1 车）→ 不触发多敌避让
  Perception f = Frame(10, "s", 5, 5, {RobotE(6, 6, "B")});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move" || c.action == "attack" || c.action == "defend" ||
          c.action == "wait", c);
  CHECK_C(c.reason.find("多敌聚集") == std::string::npos, c);
}

// ---- 37–39 组：10/28 晚 R3-A 主动换血（用户拍板：该激进就大胆，主动寻求换血） ----

void Test37_EqualHpTradesNow() {
  std::printf("[TEST] 37 同血即打（R3-A）→ 均势非孤立目标也换血\n");
  CombatStrategy s;
  // 我方 100 / B 100，直线 2 格；C 与 B 相邻（Chebyshev ≤2）→ B 不孤立，
  // 旧"严格领先一枪"不放行；R3-A 同血即打 → attack right
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(7, 5, "B"), RobotE(7, 6, "C")});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);
}

void Test38_HuntSingleEnemyWhenHealthy() {
  std::printf("[TEST] 38 满血单敌追猎（R3-A + V7 参战门）→ 游走兜底主动逼近\n");
  CombatStrategy s;
  // 任务全清 + 资源饱和（参战开关开）、我方 100 血；斜对角单敌 (6,6)（不在
  // 枪线）→ 不撤离不避让，游走应主动逼近（四邻域最小 Chebyshev 距：
  // down 与 right 并列 → 取 down）
  Perception f = Frame(10, "s", 5, 5, {RobotE(6, 6, "B")});
  f.score.resource_value = 120;  // 资源饱和 → 参战开关开（V7 前置）
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "down", c);
  CHECK_C(c.reason.find("追猎") != std::string::npos, c);
}

void Test39_NoHuntWhenNotHealthy() {
  std::printf("[TEST] 39 低血不追猎（R3-A）→ 保持原游走\n");
  CombatStrategy s;
  // 我方 50 血（< 60 追猎门槛、≥ 40 不进撤离态）+ 斜对角单敌 → 不追猎，
  // 走原游走兜底（朝中心）
  Perception f = Frame(10, "s", 5, 5, {RobotE(6, 6, "B")});
  f.self.hp = 50;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("追猎") == std::string::npos, c);
}

// ---- 40 组：10/29 V4 任务销账（裁判全程保留 missions 列表 → 完成后不再回头） ----

void Test40_CompletedMissionNotRevisited() {
  std::printf("[TEST] 40 已完成 reach 任务不再推进（V4）→ 不朝旧任务点回头\n");
  CombatStrategy s;
  // 复刻 10/29 复盘：任务点 (5,9) 已完成但仍留在 missions 列表（裁判行为），
  // 我方资源已饱和。旧代码朝 (5,9) 反复推进；V4 过滤 completed_ids 后应
  // 落入游走兜底（不再出现"朝任务点"推进）。
  Perception f = Frame(10, "s", 5, 5);
  f.missions = {ReachMission("m1", 5, 9)};
  f.score.mission_completed_ids = {"m1"};
  f.score.resource_value = 120;  // 饱和：任务本应优先（若任务未销账）
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("朝任务点") == std::string::npos, c);

  // 反例护栏：同场景但未完成（不在 completed_ids）→ 任务照常推进，
  // 防止过滤条件写反/过宽把有效任务也滤掉。
  Perception f2 = Frame(11, "s", 5, 5);
  f2.missions = f.missions;
  f2.score.resource_value = 120;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.reason.find("朝任务点") != std::string::npos, c2);
}

// ---- 41–42 组：10/29 V5 环形巡逻（用户拍板"金边银肚皮草角"：圆角方形环，角切角） ----

void Test41_RingPatrolFollowsRing() {
  std::printf("[TEST] 41 环形巡逻（V5/V8.2 R5-K 方环）→ 无任务无资源时沿环顺时针推进，不朝中心\n");
  CombatStrategy s;
  // 18×18、D=5：方环环首路点 (5,5)。本车站在环上 (6,5) → 推进到下一路点
  // (7,5) → right
  Perception f = Frame(10, "s", 6, 5);
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "right", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
  CHECK_C(c.reason.find("朝地图中心") == std::string::npos, c);

  // 地图角落区（旧中心游走会朝右下方中心走）：(5,5) 即方环环首 → 沿环推进
  Perception f2 = Frame(11, "s", 5, 5);
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.reason.find("环形巡逻") != std::string::npos, c2);
}

void Test42_RingPatrolSkipsBlockedWaypoint() {
  std::printf("[TEST] 42 环形巡逻路点被挡（V5）→ 顺环跳过，不撞墙不回头中心\n");
  CombatStrategy s;
  // 站在环首 (6,5)，下一路点 (7,5) 是障碍 → 顺环找下一个可达路点，
  // 仍走环形巡逻（绕行），不允许退回"朝地图中心"游走。
  Perception f = Frame(10, "s", 6, 5, {ObstacleE(7, 5)});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);

  // 连续帧推进：不允许停在原地裸 wait 或退回中心游走
  bool progressed = false;
  for (int t = 11; t <= 15; ++t) {
    Perception f3 = Frame(t, "s", 6, 5, {ObstacleE(7, 5)});
    ActionChoice c3 = s.decide(f3, nullptr);
    CHECK_C(c3.action == "move", c3);
    if (c3.reason.find("环形巡逻") != std::string::npos) progressed = true;
  }
  CHECK(progressed);
}

// ---- 43–46 组：10/29 V6 低血不逃命 + 巡逻脱环修复（用户实测 500ms 场：
//      左上环段 58/50/48 次、右侧/下边各 1–3 次 —— 脱环就近吸附致原地徘徊；
//      低血整场 109 tick 游走 + 95 tick 撤离、伤害仅 20 —— 永久逃命不得分） ----

void Test43_RingPatrolNoNearestSnap() {
  std::printf("[TEST] 43 脱环不回吸（V6）→ 保持环序推进，不吸附最近路点\n");
  CombatStrategy s;
  // t1：站在环上 (6,5) → 沿环推进到 (7,5)（patrol_idx 记住进度）
  Perception f1 = Frame(10, "s", 6, 5);
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.reason.find("[7,5]") != std::string::npos, c1);
  // t2：脱环到环内侧 (6,6)（方环内侧第一格，模拟采集/避火单步中断）。V5 会
  // 吸附最近路点 [6,5]（左上角徘徊根源）；V6 保持环序 → 目标仍是 [7,5]
  Perception f2 = Frame(11, "s", 6, 6);
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.reason.find("[7,5]") != std::string::npos, c2);
  CHECK_C(c2.reason.find("[6,5]") == std::string::npos, c2);
}

void Test44_RespawnReanchorsNearest() {
  std::printf("[TEST] 44 重生就近归环（V6）→ 复活后吸附最近路点 [5,12]\n");
  CombatStrategy s;
  // t1：阵亡 → wait（记 alive_prev=false）
  Perception f1 = Frame(10, "s", 6, 5);
  f1.self.alive = false;
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "wait", c1);
  // t2：复活在右下出生区 (1,17) → 就近归环：最近可达路点 [5,12]
  // （曼哈顿 9，V8.2 方环含直角），不再追左上角旧进度
  Perception f2 = Frame(11, "s", 1, 17);
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.reason.find("[5,12]") != std::string::npos, c2);
}

void Test45_LowHpWinningTradeAttacks() {
  std::printf("[TEST] 45 低血领先一枪照打（V6）→ 30 vs 25 attack\n");
  CombatStrategy s;
  // 30 vs 25：R1 生存门放行（反打 20 < 30）；V6 低血不再整块跳过第二遍
  // 换血（V5/P3 会跳 → 低血只收头，实战伤害掉到 20）
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(7, 5, "B", true, false, false, 25)});
  f.self.hp = 30;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);
}

void Test46_LowHpIdleKeepsScoring() {
  std::printf("[TEST] 46 低血脱险不逃命（V6）→ 敌车消失后走巡逻环，不再低血游走\n");
  CombatStrategy s;
  // t1：hp=20、B 在同排 3 格（枪线上）→ 低血避火（垂直脱线）
  Perception f1 = Frame(10, "s", 5, 5, {RobotE(8, 5, "B")});
  f1.self.hp =20;
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "move", c1);
  // t2：敌车出视野。V5 撤离态回滞 + 最后已知敌车 → 低血游走空转；
  // V6 直接走巡逻环继续得分
  Perception f2 = Frame(11, "s", 5, 4);
  f2.self.hp = 20;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.reason.find("环形巡逻") != std::string::npos, c2);
  CHECK_C(c2.reason.find("低血游走") == std::string::npos, c2);
}

// ---- 47–56 组：10/30 V7/R4-A 集火参与 + 参战信念门 + 防御目标落井下石
//      （用户拍板："人多聚堆就上前劝架、任务/资源分作为参战信念、防御目标照打"） ----

void Test47_ClusterFocusLowestHp() {
  std::printf("[TEST] 47 聚堆集火参与（R4-A）→ 逼近最低血目标，不逃跑\n");
  CombatStrategy s;
  // 任务全清 + 资源饱和（参战开）；B(4,3) hp=30、C(6,4) hp=90 均在视野内
  // 且斜对角不在枪线 → 集火参与选最低血 B，四邻域唯一逼近格 up
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(4, 3, "B", true, false, false, 30),
                        RobotE(6, 4, "C", true, false, false, 90)});
  f.score.resource_value = 120;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up", c);
  CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
  CHECK_C(c.reason.find("HP 30") != std::string::npos, c);
}

void Test48_NoEngageWhenResourceLeft() {
  std::printf("[TEST] 48 参战门关（资源未采完）→ 多敌不集火也不再逃跑（V8.2 R5-L）\n");
  CombatStrategy s;
  // 无未完成任务，但视野内有资源（被障碍围死，采集器不可达 → 落到游走层）
  // → 资源没拿完，不得集火参与；R5-L 删除避让后照常巡逻（不逃）
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 6, "B"), RobotE(6, 4, "C"),
                        ResourceE(8, 8, 10, "r1"), ObstacleE(7, 8),
                        ObstacleE(9, 8), ObstacleE(8, 7), ObstacleE(8, 9)});
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
  CHECK_C(c.reason.find("多敌聚集") == std::string::npos, c);
  CHECK_C(c.reason.find("集火参与") == std::string::npos, c);
}

void Test49_DamageMissionUnlocksEngage() {
  std::printf("[TEST] 49 战斗任务在身 → 资源未饱和也参战（R4-A）\n");
  CombatStrategy s;
  // 未完成 effective_damage 任务 = 战斗任务开锁；资源 0（未饱和）也应集火
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(4, 3, "B", true, false, false, 30),
                        RobotE(6, 4, "C", true, false, false, 90)});
  f.score.resource_value = 0;
  f.missions = {DamageMission("d1")};
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up", c);
  CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
}

void Test50_DefendingLowestHpSelected() {
  std::printf("[TEST] 50 防御中最低血目标照选（R4-A 落井下石）→ 逼近并标记\n");
  CombatStrategy s;
  // 最低血 B(4,3) hp=30 正在防御 → 照选（落井下石），逼近 up
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(4, 3, "B", true, true, false, 30),
                        RobotE(6, 4, "C", true, false, false, 90)});
  f.score.resource_value = 120;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up", c);
  CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
  CHECK_C(c.reason.find("落井下石") != std::string::npos, c);
}

void Test51_ProtectedTargetSkipped() {
  std::printf("[TEST] 51 保护期目标不可伤 → 跳过，选次低血目标\n");
  CombatStrategy s;
  // 最低血 B(4,3) hp=10 在保护期（不可伤）→ 跳过，选 C(6,4) hp=90
  // （reason 应记 HP 90 而非 HP 10）；C 斜邻 → 摆正枪线步 up
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(4, 3, "B", true, false, true, 10),
                        RobotE(6, 4, "C", true, false, false, 90)});
  f.score.resource_value = 120;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up", c);
  CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
  CHECK_C(c.reason.find("HP 90") != std::string::npos, c);
}

void Test52_LowHpNoClusterEngage() {
  std::printf("[TEST] 52 低血不参与集火（R4-A 安全门①）→ 低血避火接管\n");
  CombatStrategy s;
  // 参战开但 hp=35（< 2×attack_damage）→ 不集火；两敌近身 → 低血避火
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 6, "B", true, false, false, 20),
                        RobotE(6, 4, "C", true, false, false, 90)});
  f.score.resource_value = 120;
  f.self.hp = 35;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("集火参与") == std::string::npos, c);
}

void Test53_SaturatedStillFocuses() {
  std::printf("[TEST] 53 对抗分饱和后照常集火（R5-N 撤掉安全门③）→ 无可击杀也追\n");
  CombatStrategy s;
  // 参战开 + 对抗分饱和（500）且无战斗任务：R5-N 前"只收头"→ 不集火；
  // 现在 hp 60 目标照追（击杀扣对手 D，相对排名仍值钱）
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(4, 2, "B", true, false, false, 60),
                        RobotE(9, 9, "C", true, false, false, 80)});
  f.score.resource_value = 120;
  f.score.effective_damage = 500;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
  CHECK_C(c.reason.find("HP 60") != std::string::npos, c);

  // 同场次下一 Tick：出现 hp=15 可击杀目标 → 恢复集火（收头扣对手 D）
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(4, 3, "B", true, false, false, 15),
                         RobotE(9, 9, "C", true, false, false, 90)});
  f2.score.resource_value = 120;
  f2.score.effective_damage = 500;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "up", c2);
  CHECK_C(c2.reason.find("集火参与") != std::string::npos, c2);
}

void Test54_SurvivalGateBlocksPursuit() {
  std::printf("[TEST] 54 本 Tick 枪线必死不追（R4-A 安全门②，R1 生存门）→ 必死还手\n");
  CombatStrategy s;
  // hp=40、四路枪线（k_in=4 → 4×20=80 ≥ 40 必死）；防御减半 40 也活不了
  // （无防御兜底）→ 集火参与被生存门拦下；V8.3 R5-O 起不再白走一步：
  // 本 Tick 必倒时先还一炮（移动救不了命，死后 3 Tick 复活、走位无意义）
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 5, "B"), RobotE(7, 5, "C"),
                        RobotE(4, 5, "D"), RobotE(3, 5, "E")});
  f.score.resource_value = 120;
  f.self.hp = 40;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.reason.find("集火参与") == std::string::npos, c);
  CHECK_C(c.reason.find("必死还手") != std::string::npos, c);
}

void Test55_ThreatStepGateAtHp50() {
  std::printf("[TEST] 55 逼近步新增枪线且 HP<60 → 放弃该步（R4-A 安全门④）\n");
  CombatStrategy s;
  // T(4,4) hp=40 斜对角目标（不在枪线，攻击层不打；V8.2 R5-J 追猎门槛=血高于
  // 目标，50 > 40 放行）；G(3,5) hp=100 同排枪线上（cur_t=1）；上方 (5,4) 被
  // 障碍堵死。唯一逼近步 left (4,5) 会把枪线威胁加到 2（T+G 同列夹击），
  // hp=50 < 60 → 拒绝 → 无逼近格 → 不集火，走环形巡逻
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(4, 4, "B", true, false, false, 40),
                        RobotE(3, 5, "C", true, false, false, 100),
                        ObstacleE(5, 4)});
  f.score.resource_value = 120;
  f.self.hp = 50;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("集火参与") == std::string::npos, c);

  // 同场景 hp=60（≥ 3×attack_damage，威胁门④放行）→ 允许顶线压上 → left 集火
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(4, 4, "B", true, false, false, 40),
                         RobotE(3, 5, "C", true, false, false, 100),
                         ObstacleE(5, 4)});
  f2.score.resource_value = 120;
  f2.self.hp = 60;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "left", c2);
  CHECK_C(c2.reason.find("集火参与") != std::string::npos, c2);
}

void Test56_DiagonalAdjacentAlignsGunline() {
  std::printf("[TEST] 56 斜邻目标先摆正枪线（R4-A 逼近判定）→ up 后进入射程\n");
  CombatStrategy s;
  // 最低血 B(6,4) 与我方斜邻（Chebyshev 1、不可攻击）→ 一步摆正枪线即进
  // 1 格射程：up 与 right 并列 → 取 up（方向字典序）
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 4, "B", true, false, false, 30),
                        RobotE(7, 7, "C", true, false, false, 90)});
  f.score.resource_value = 120;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up", c);
  CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
}

// ---- 47b/48c + 57–65 组：10/30 V8 R5-A~E（V7 首场练习赛实测修复 + 用户拍板：
//      资源采尽即饱和 / 幻觉自适应 / 鬼墙徘徊修复 / 巡逻锚定劫持修复 /
//      残血回本敢死队：每条命赚 ≥80 就回本，不用非要避战） ----

void Test47b_ResourcesExhaustedAfterLapUnlocksEngage() {
  std::printf("[TEST] 47b 巡逻覆盖满一圈且资源采尽 → 参战锁存开门（V8 R5-F/R5-G）\n");
  CombatStrategy s;
  // t1..t28：沿 18×18 环走满全部 28 个环格（R5-G：踩格覆盖 100% ≥ 90% 门槛）；
  // 且无可见资源、信念记忆空 → 资源侧锁存 → 参战开关开
  int t = 10;
  WalkRing(s, t);
  // t2：聚堆 + 资源分 0（未达饱和线）→ 已锁存 → 集火参与（无覆盖实证则避让）
  Perception f2 = Frame(t++, "s", 5, 5,
                        {RobotE(4, 3, "B", true, false, false, 30),
                         RobotE(6, 4, "C", true, false, false, 90)});
  f2.score.resource_value = 0;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "up", c2);
  CHECK_C(c2.reason.find("集火参与") != std::string::npos, c2);
}

void Test48c_RememberedResourceKeepsEngageOff() {
  std::printf("[TEST] 48c 记忆资源未采尽（V8 R5-A）→ 视野外消失不入账 → 不集火（V8.2 R5-L 不再逃跑）\n");
  CombatStrategy s;
  // t1：资源 (8,8) 可见入记忆，但被障碍围死（采集器不可达 → 游走层接管）；
  // t2：自车在 (0,0)，资源出视野（未确认被采）→ 记忆保留 → 资源未采尽 →
  // 2 敌聚堆不集火、也不逃跑（R5-L），照常巡逻
  Perception f1 = Frame(10, "s", 5, 5,
                        {ResourceE(8, 8, 10, "r1"), ObstacleE(7, 8),
                         ObstacleE(9, 8), ObstacleE(8, 7), ObstacleE(8, 9)});
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s", 0, 0,
                        {RobotE(1, 1, "B"), RobotE(2, 2, "C")});
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
  CHECK_C(c.reason.find("多敌聚集") == std::string::npos, c);
}

void Test57_PaidLifeTradesAtLowHp() {
  std::printf("[TEST] 57 本命回本低血换血（V8 R5-E）→ 20 vs 30 attack\n");
  CombatStrategy s;
  // t1：基线 100（本命起点）；t2：净伤害 185 → 本命赚 85 ≥ 80 回本 →
  // 20 vs 30 非击杀目标也换血（未回本则 safe_trade 拦下 → defend）
  Perception f1 = Frame(10, "s", 5, 5);
  f1.score.effective_damage = 100;
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(7, 5, "B", true, false, false, 30)});
  f2.score.effective_damage = 185;
  f2.self.hp = 20;
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);
}

void Test58_PaidLifeTakesLethalKill() {
  std::printf("[TEST] 58 回本后同归于尽照收（V8 R5-E）→ 20 vs 16 attack\n");
  CombatStrategy s;
  // 20 vs 16：一枪收头但同 Tick 反打 20 必死（同归于尽净 0）；本命已回本
  // （185-100=85）→ R1 生存门放行 → attack（未回本则 defend，见 59）
  Perception f1 = Frame(10, "s", 5, 5);
  f1.score.effective_damage = 100;
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(7, 5, "B", true, false, false, 16)});
  f2.score.effective_damage = 185;
  f2.self.hp = 20;
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);
}

void Test59_UnpaidLifeStillDefends() {
  std::printf("[TEST] 59 未回本同归于尽不收（R1 生存门保持）→ 20 vs 16 defend\n");
  CombatStrategy s;
  Perception f1 = Frame(10, "s", 5, 5);
  f1.score.effective_damage = 100;
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(7, 5, "B", true, false, false, 16)});
  f2.score.effective_damage = 130;  // 本命赚 30 < 80 未回本
  f2.self.hp = 20;
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "defend", c);
}

void Test60_PaidLifeNoFlee() {
  std::printf("[TEST] 60 回本后低血不逃命（V8 R5-E）→ 跳过低血避火主动参战\n");
  CombatStrategy s;
  // hp=20 回本（基线 100 → 185）；两敌斜对角近身：未回本会低血避火，
  // 回本 → 跳过避火 → 集火参与逼近（参与门槛已绕过）
  Perception f1 = Frame(10, "s", 5, 5);
  f1.score.effective_damage = 100;
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(4, 3, "B", true, false, false, 30),
                         RobotE(6, 4, "C", true, false, false, 90)});
  f2.score.effective_damage = 185;
  f2.score.resource_value = 120;
  f2.self.hp = 20;
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
  CHECK_C(c.reason.find("低血避火") == std::string::npos, c);
}

void Test61_IllusionFlipsMove() {
  std::printf("[TEST] 61 幻觉生效（V8 R5-B）→ move 方向反向发出\n");
  CombatStrategy s;
  // t1：正常巡逻（发出 right）；t2：回执显示实走 left（发 right 实走 left）
  // → 检测到幻觉 → 本帧意图 right 反向发出为 left
  Perception f1 = Frame(10, "s", 5, 5);
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "move", c1);
  Perception f2 = Frame(11, "s", 5, 5);
  f2.self.has_last_action = true;
  f2.self.last_tick = 10;
  f2.self.last_action = "move";
  f2.self.last_moved = true;
  f2.self.has_last_direction = true;
  f2.self.last_effective_direction = "left";  // 与发出方向相反
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "left", c2);  // 意图 right → 反向 left
}

void Test62_IllusionStableThenReleased() {
  std::printf("[TEST] 62 幻觉补偿稳定 + 解除（V8 R5-B）→ 持续反转，回执一致即恢复\n");
  CombatStrategy s;
  Perception f1 = Frame(10, "s", 5, 5);
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s", 5, 5);
  f2.self.has_last_action = true;
  f2.self.last_tick = 10;
  f2.self.last_action = "move";
  f2.self.last_moved = true;
  f2.self.has_last_direction = true;
  f2.self.last_effective_direction = "left";
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.direction == "left", c2);  // 幻觉生效，反向发出
  // t3：回执 = 反向(发出 left → 实走 right)，幻觉仍在 → 继续反向发出 left
  Perception f3 = Frame(12, "s", 5, 5);
  f3.self.has_last_action = true;
  f3.self.last_tick = 11;
  f3.self.last_action = "move";
  f3.self.last_moved = true;
  f3.self.has_last_direction = true;
  f3.self.last_effective_direction = "right";
  ActionChoice c3 = s.decide(f3, nullptr);
  CHECK_C(c3.direction == "left", c3);  // 不振荡，保持补偿
  // t4：回执 = 发出方向（幻觉解除）→ 恢复真实方向 right
  Perception f4 = Frame(13, "s", 5, 5);
  f4.self.has_last_action = true;
  f4.self.last_tick = 12;
  f4.self.last_action = "move";
  f4.self.last_moved = true;
  f4.self.has_last_direction = true;
  f4.self.last_effective_direction = "left";  // 等于上帧发出
  ActionChoice c4 = s.decide(f4, nullptr);
  CHECK_C(c4.direction == "right", c4);
}

void Test63_IllusionDoesNotFlipAttack() {
  std::printf("[TEST] 63 幻觉不影响攻击（V8 R5-B）→ attack 方向照常\n");
  CombatStrategy s;
  // t1 正常帧（发出 right）；t2 幻觉回执 + 敌车直线 2 格 → attack right
  // （攻击不受方向反转影响，10/30 实测 t56–59 攻击方向照常命中）
  Perception f1 = Frame(10, "s", 5, 5);
  (void)s.decide(f1, nullptr);
  Perception f2 = Frame(11, "s", 5, 5,
                        {RobotE(7, 5, "B", true, false, false, 50)});
  f2.self.has_last_action = true;
  f2.self.last_tick = 10;
  f2.self.last_action = "move";
  f2.self.last_moved = true;
  f2.self.has_last_direction = true;
  f2.self.last_effective_direction = "left";
  ActionChoice c = s.decide(f2, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);
}

void Test64_ObstacleForgottenWhenGone() {
  std::printf("[TEST] 64 障碍遗忘（V8 R5-C）→ 视野内消失的临时墙不再挡路\n");
  CombatStrategy s;
  // t1：(6,5) 在环上，(7,5) 是障碍 → 顺环跳过到 (8,5)（BFS 绕行）；
  // t2：同视野内障碍消失（WorldSplit 临时墙消失）→ 遗忘 → BFS 直行 right
  Perception f1 = Frame(10, "s", 6, 5, {ObstacleE(7, 5)});
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "move", c1);
  Perception f2 = Frame(11, "s", 6, 5);
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "right", c2);  // 未遗忘则绕行 up/down
}

void Test65_PatrolAnchorOnlyOnArrival() {
  std::printf("[TEST] 65 巡逻锚定不劫持（V8 R5-D）→ 绕行途中保持原路点\n");
  CombatStrategy s;
  // t1：(9,12) 在环上，(8,12) 被堵 → 顺环跳过 → 目标 [7,12]（BFS 绕行）；
  // t2：绕行途中经过环格 (12,9)（旧逻辑劫持锚定到 [12,10]）→ 保持 [7,12]
  Perception f1 = Frame(10, "s", 9, 12, {ObstacleE(8, 12)});
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "move", c1);
  CHECK_C(c1.reason.find("[7,12]") != std::string::npos, c1);
  Perception f2 = Frame(11, "s", 12, 9, {ObstacleE(8, 12)});
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.reason.find("[7,12]") != std::string::npos, c2);
  CHECK_C(c2.reason.find("[12,10]") == std::string::npos, c2);
}

void Test66_NoEngageBeforeFullLap() {
  std::printf("[TEST] 66 未绕满一圈不锁存（V8 R5-F）→ 不集火也不逃跑（V8.2 R5-L）\n");
  CombatStrategy s;
  // 直接聚堆、无任务、资源分 0、记忆空：还没绕满一圈 → 不能断定地图无资源
  // （角落可能藏资源）→ 不得集火（对照 47b 先有满圈实证）；R5-L 后照常巡逻
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 6, "B"), RobotE(6, 4, "C")});
  f.score.resource_value = 0;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
  CHECK_C(c.reason.find("多敌聚集") == std::string::npos, c);
  CHECK_C(c.reason.find("集火参与") == std::string::npos, c);
}

void Test67_LatchedEngageSurvivesResourceSighting() {
  std::printf("[TEST] 67 锁存后偶见资源不影响参战（V8 R5-F/R5-G）→ 顺手捡/不可达都回集火\n");
  CombatStrategy s;
  // t1..t28：走满全环 → 覆盖率 100% → 参战门判定在巡逻前，锁存顺延到下一帧
  int t = 10;
  WalkRing(s, t);
  // t2：无资源聚堆 → 资源采尽 + 覆盖满 → 锁存 → 集火参与
  Perception f2 = Frame(t++, "s", 5, 5,
                        {RobotE(4, 3, "B", true, false, false, 30),
                         RobotE(6, 4, "C", true, false, false, 90)});
  f2.score.resource_value = 0;
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "up", c2);
  CHECK_C(c2.reason.find("集火参与") != std::string::npos, c2);
  // t3：资源突然可见（被障碍围死、采集器不可达）→ 已锁存 → 游走层仍集火
  // 参与，不因资源出现切回避让（资源可捡时采集层优先级更高，会先顺手捡）
  Perception f3 = Frame(12, "s", 5, 5,
                        {RobotE(4, 3, "B", true, false, false, 30),
                         RobotE(6, 4, "C", true, false, false, 90),
                         ResourceE(8, 8, 10, "r9"), ObstacleE(7, 8),
                         ObstacleE(9, 8), ObstacleE(8, 7), ObstacleE(8, 9)});
  f3.score.resource_value = 0;
  ActionChoice c3 = s.decide(f3, nullptr);
  CHECK_C(c3.action == "move", c3);
  CHECK_C(c3.direction == "up", c3);
  CHECK_C(c3.reason.find("集火参与") != std::string::npos, c3);
  // t4：资源消失（被捡走），敌车仍在 → 继续集火，不回退避让
  Perception f4 = Frame(13, "s", 5, 5,
                        {RobotE(4, 3, "B", true, false, false, 30),
                         RobotE(6, 4, "C", true, false, false, 90)});
  f4.score.resource_value = 0;
  ActionChoice c4 = s.decide(f4, nullptr);
  CHECK_C(c4.reason.find("集火参与") != std::string::npos, c4);
  CHECK_C(c4.reason.find("多敌聚集") == std::string::npos, c4);
}

void Test68_MostRingCoverageLatchesEngage() {
  std::printf("%s", "[TEST] 68 覆盖 ≥90% 环格即锁存（V8 R5-G）→ 敌占环格踩不满 100% 也开门\n");
  CombatStrategy s;
  // 只踩 26/28 ≈ 93% ≥ 90% 门槛（敌车长期占住个别环格的兜底）→ 锁存
  int t = 10;
  WalkRing(s, t, 26);
  Perception f = Frame(t, "s", 5, 5,
                       {RobotE(4, 3, "B", true, false, false, 30),
                        RobotE(6, 4, "C", true, false, false, 90)});
  f.score.resource_value = 0;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up", c);
  CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
}

void Test69_BelowCoverageThresholdNoEngage() {
  std::printf("%s", "[TEST] 69 覆盖不足 90% 不锁存（V8 R5-G）→ 不集火也不逃跑（V8.2 R5-L）\n");
  CombatStrategy s;
  // 只踩 20/28 ≈ 71% < 90% 门槛（角落还有环段没巡到，可能藏资源）→ 不锁存，
  // 无资源聚堆仍不集火（对照 68 覆盖足够才开门）；R5-L 后照常巡逻
  int t = 10;
  WalkRing(s, t, 20);
  Perception f = Frame(t, "s", 5, 5,
                       {RobotE(6, 6, "B"), RobotE(6, 4, "C")});
  f.score.resource_value = 0;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
  CHECK_C(c.reason.find("多敌聚集") == std::string::npos, c);
  CHECK_C(c.reason.find("集火参与") == std::string::npos, c);
}

void Test70_HpDominanceEngagesDespiteClosedGate() {
  std::printf("[TEST] 70 血量碾压参战（V8.1 R5-H / V8.2 R5-J 血高即追）→ 参战门关也集火/追猎\n");
  // ① 我方 100 vs 敌 30/90：经济门关（无覆盖实证、未饱和）→ 血量优势
  // （100 > 90 最强敌车，R5-J）→ 集火参与最低血 B(30) → up
  {
    CombatStrategy s;
    Perception f = Frame(10, "s", 5, 5,
                         {RobotE(4, 3, "B", true, false, false, 30),
                          RobotE(6, 4, "C", true, false, false, 90)});
    f.score.resource_value = 0;
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    CHECK_C(c.direction == "up", c);
    CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
    CHECK_C(c.reason.find("多敌聚集") == std::string::npos, c);
  }
  // ② 我方 60 vs 敌 100/100：血量不高于对方（60 < 100）→ 不集火；R5-L 后也
  //    不再逃跑，照常巡逻
  {
    CombatStrategy s;
    Perception f = Frame(10, "s", 5, 5,
                         {RobotE(6, 6, "B"), RobotE(6, 4, "C")});
    f.self.hp = 60;
    f.score.resource_value = 0;
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
    CHECK_C(c.reason.find("多敌聚集") == std::string::npos, c);
    CHECK_C(c.reason.find("集火参与") == std::string::npos, c);
  }
  // ③ 单敌 40 斜邻：血高（100 > 40）→ 单敌追猎（R3-A 挂同一参战门）
  {
    CombatStrategy s;
    Perception f = Frame(10, "s", 5, 5, {RobotE(6, 6, "B", true, false, false, 40)});
    f.score.resource_value = 0;
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    CHECK_C(c.reason.find("单敌追猎") != std::string::npos, c);
  }
}

void Test71_RingFollowingAlongRing() {
  std::printf("[TEST] 71 环上沿环推进（V8.1 R5-I）→ 不 BFS 直线切腹地\n");
  CombatStrategy s;
  // 先沿方环走 12 格到 (12,9)（指针推进到 (12,10)），再把车放回 (9,5)——
  // 模拟被劫持后回到环上落后段：旧逻辑 BFS 从 (9,5) 直插 (12,10)（第一步
  // down 切腹地）；R5-I 应沿环顺时针走 → right 到 (10,5)
  int t = 10;
  WalkRing(s, t, 12);
  Perception f = Frame(t, "s", 9, 5);
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "right", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
}

void Test72_TeleportRespawnReanchorsNearRing() {
  std::printf("[TEST] 72 瞬移复活就近归环（V8.1 R5-I）→ 不再直插对岸路点\n");
  CombatStrategy s;
  // 沿环走 12 格 → 指针在 (12,10)；车瞬移到 (1,1)（裁判跳过死亡帧直接满血
  // 复活在出生点，V8 实测 tick 136）→ 视同重生就近归环：目标应换成最近环点
  // [5,5]（曼哈顿 8，方环环首），而不是对岸 [12,10] 横穿全图
  int t = 10;
  WalkRing(s, t, 12);
  Perception f = Frame(t, "s", 1, 1);
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("[5,5]") != std::string::npos, c);
  CHECK_C(c.reason.find("[12,10]") == std::string::npos, c);
}

void Test73_HuntWheneverHpHigher() {
  std::printf("[TEST] 73 血高即追（V8.2 R5-J）→ 砍掉 3×damage/2×最低血高门槛\n");
  // ① 我方 40 vs 单敌 30 斜邻：旧门槛（≥3×damage=60）会拦 → 现在血高即追猎
  {
    CombatStrategy s;
    Perception f = Frame(10, "s", 5, 5, {RobotE(6, 6, "B", true, false, false, 30)});
    f.self.hp = 40;
    f.score.resource_value = 0;
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    CHECK_C(c.direction == "down", c);
    CHECK_C(c.reason.find("单敌追猎") != std::string::npos, c);
  }
  // ② 我方 100 vs 单敌 95：旧门槛（≥2×最低血=190）会拦 → 现在血高即追猎
  {
    CombatStrategy s;
    Perception f = Frame(10, "s", 5, 5, {RobotE(6, 6, "B", true, false, false, 95)});
    f.score.resource_value = 0;
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    CHECK_C(c.reason.find("单敌追猎") != std::string::npos, c);
  }
  // ③ 我方 60 vs 敌 50/55：旧门槛（≥2×最低血=100）会拦 → 现在血高于最强敌
  // 即集火最低血 B(50) → up
  {
    CombatStrategy s;
    Perception f = Frame(10, "s", 5, 5,
                         {RobotE(4, 3, "B", true, false, false, 50),
                          RobotE(6, 4, "C", true, false, false, 55)});
    f.self.hp = 60;
    f.score.resource_value = 0;
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    CHECK_C(c.direction == "up", c);
    CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
  }
  // ④ 我方 45 vs 单敌 50：血不高于对方 → 不追猎（保持原游走/巡逻）
  {
    CombatStrategy s;
    Perception f = Frame(10, "s", 5, 5, {RobotE(6, 6, "B", true, false, false, 50)});
    f.self.hp = 45;
    f.score.resource_value = 0;
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    CHECK_C(c.reason.find("追猎") == std::string::npos, c);
  }
}

// ---- 88–89 组：10/31 V8.6 资源优先（用户："最重要的是资源，哪怕为此掉血，
//      只要不死也要多捡"） ----

// 88：资源未拿满、敌车不在 2 格内 → 血量压过对面也不主动追，先去捡资源。
void Test88_ResourcePrioritySuppressesHunt() {
  std::printf("[TEST] 88 资源优先（V8.6）→ 血量压过远敌也不追，先采集\n");
  CombatStrategy s;
  // 100 vs 90（hp_dominant 成立），但敌车在 3 格外、视野内有资源 → 不追猎
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(8, 5, "B", true, false, false, 90),
                        ResourceE(4, 7, 10, "r1")});
  f.score.resource_value = 0;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("采集") != std::string::npos, c);
  CHECK_C(c.reason.find("单敌追猎") == std::string::npos, c);
}

// 91：资源分已饱和、伤害未饱和、无资源任务 → 不再为不涨分的资源跑图，回巡逻。
void Test91_SaturatedResourcesYieldToCombat() {
  std::printf("[TEST] 91 资源饱和+伤害未满 → 停止捡不涨分的资源\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {ResourceE(5, 9, 10, "r1")});
  f.score.resource_value = 120;
  f.score.effective_damage = 0;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
}

// 90：资源分已饱和但 high_resources 任务未完成 → 仍探索找高价值资源，不巡逻。
void Test90_ResourceMissionKeepsExploring() {
  std::printf("[TEST] 90 资源任务未完成（分已饱和）→ 仍探索找资源\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5);
  f.score.resource_value = 120;  // 资源分已满
  rmtc_bot::Mission m;
  m.id = "h1";
  m.type = "high_resources";
  m.has_target = true;
  m.target = 1;
  f.missions = {m};
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("采集") != std::string::npos, c);
}

// 92：10/31 用户再次确认"超过 500 凭什么不追"——对抗分饱和只影响自己那 15 分
// 封顶，同分比较仍用未截断的有效伤害，且每杀一次对手扣 60 D（相对排名有效）。
// 这里特意保留一个未完成、无坐标的非战斗任务（关掉"任务全清"那条参战路），
// 资源分已饱和（关掉资源优先），只留 hp_dominant 一条路：饱和前后都应追。
void Test92_SaturatedStillChasesWithPendingMission() {
  std::printf("[TEST] 92 饱和后仍追猎（R5-N 不得回退）→ 同分比较/击杀扣 D 仍值钱\n");
  rmtc_bot::Mission pending;
  pending.id = "m1";
  pending.type = "defense";  // 无坐标：try_mission 跳过；仅让 no_pending_missions=false
  // ① 单敌：资源饱和 + 伤害 500 + 任务在身，hp_dominant 仍是唯一参战理由
  {
    CombatStrategy s;
    Perception f = Frame(10, "s", 5, 5,
                         {RobotE(8, 5, "B", true, false, false, 70)});
    f.score.resource_value = 120;
    f.score.effective_damage = 500;
    f.missions = {pending};
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    CHECK_C(c.direction == "right", c);
    CHECK_C(c.reason.find("单敌追猎") != std::string::npos, c);
  }
  // ② 多敌：同样条件下集火最低血目标（击杀 = 对手 -60 D，仍要打）
  {
    CombatStrategy s;
    Perception f = Frame(10, "s", 5, 5,
                         {RobotE(8, 5, "B", true, false, false, 60),
                          RobotE(9, 9, "C", true, false, false, 90)});
    f.score.resource_value = 120;
    f.score.effective_damage = 500;
    f.missions = {pending};
    ActionChoice c = s.decide(f, nullptr);
    CHECK_C(c.action == "move", c);
    CHECK_C(c.direction == "right", c);
    CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
    CHECK_C(c.reason.find("HP 60") != std::string::npos, c);
  }
}

// 89：低血 30、敌车 2 格直线（本 Tick 20 伤，不死）→ 允许走进枪线捡资源；
// 旧 V6"威胁不增"门会放弃这 10 分。
void Test89_ResourceStepUnderFireIfSurvivable() {
  std::printf("[TEST] 89 可承受掉血仍去采集（V8.6）→ 30 血吃 20 伤换资源\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(7, 5, "B", true, false, false, 90),
                        ResourceE(6, 5, 10, "r1")});
  f.self.hp = 30;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "right", c);
  CHECK_C(c.reason.find("采集") != std::string::npos, c);
}

}  // namespace

// ---- 74–77 组：10/30 V8.2 练习赛实测后用户拍板
//      "尽可能参战集火；血量低并且赚够分了那就送一波" / "在交火范围了又往前
//      走一步让别人白打一下"（R5-L 删多敌避让 / R5-M 不白挨打） ----

void Test74_MultiEnemyHpEdgeFocus() {
  std::printf("[TEST] 74 多敌血量压过最低血目标就集火（V8.2 R5-L）→ 不逃跑\n");
  CombatStrategy s;
  // 参战门关（无任务、资源 0、未满圈），但 B hp=60 低于我方 100 → 尽可能
  // 参战集火：R4-A 逼近最低血 B(4,3) → up
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(4, 3, "B", true, false, false, 60),
                        RobotE(6, 4, "C", true, false, false, 100)});
  f.score.resource_value = 0;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction == "up", c);
  CHECK_C(c.reason.find("集火参与") != std::string::npos, c);
  CHECK_C(c.reason.find("HP 60") != std::string::npos, c);
  CHECK_C(c.reason.find("多敌聚集") == std::string::npos, c);
}

void Test75_MultiEnemyNoFlee() {
  std::printf("[TEST] 75 多敌血量不占优也不逃跑（V8.2 R5-L）→ 照常巡逻\n");
  CombatStrategy s;
  // 我方 100 vs 敌 100/100（无血量便宜）→ 不集火；R5-L 后也不逃跑，环形巡逻
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 6, "B"), RobotE(6, 4, "C")});
  f.score.resource_value = 0;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
  CHECK_C(c.reason.find("多敌聚集") == std::string::npos, c);
}

void Test76_SaturatedCounterPunch() {
  std::printf("[TEST] 76 饱和时优势对射照常还手（R5-M/R5-N）→ 不再白挨打\n");
  CombatStrategy s;
  // 对抗分已满（500），B 在右 2 格（直线射程内）hp=60 < 我方 80：
  // 旧逻辑饱和只收头 → 不还手被白打；R5-M 还手、R5-N 起与未饱和完全同策略
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(7, 5, "B", true, false, false, 60)});
  f.self.hp = 80;
  f.score.effective_damage = 500;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "right", c);
}

void Test77_PatrolAvoidsEnemyRange() {
  std::printf("[TEST] 77 巡逻不踏进敌车射程（V8.2 R5-M）→ 绕开 1–2 格枪线\n");
  CombatStrategy s;
  // 本车 (7,5)，B 在同排 (10,5)（3 格，本 Tick 打不到也不还手）：巡逻默认
  // 向右 = 一步踏进 B 的 1–2 格射程白挨打；R5-M 应换安全方向
  Perception f = Frame(10, "s", 7, 5,
                       {RobotE(10, 5, "B", true, false, false, 100)});
  f.score.effective_damage = 500;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.direction != "right", c);
  CHECK_C(c.reason.find("环形巡逻") != std::string::npos, c);
}

void Test78_SaturatedSoloChase() {
  std::printf("[TEST] 78 对抗分饱和后单敌照常追猎（R5-N）→ 不再「饱和不追」\n");
  CombatStrategy s;
  // 资源饱和（参战开）+ 对抗分饱和 + 视野内 1 敌（hp 70 < 我方 100、不在枪线）
  // R5-N 前：饱和 → 不追，回环形巡逻；现在 → 主动逼近（R3-A 追猎）
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(9, 9, "B", true, false, false, 70)});
  f.score.resource_value = 120;
  f.score.effective_damage = 500;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "move", c);
  CHECK_C(c.reason.find("单敌追猎") != std::string::npos, c);
}

void Test79_SaturatedSurvivalGateHolds() {
  std::printf("[TEST] 79 饱和≠送命：R1 生存门仍在（R5-N）→ 枪线必死不追\n");
  CombatStrategy s;
  // 对抗分饱和 + hp=40、两路直线枪线（2×20=40 ≥ 40 本 Tick 必死）：
  // 不集火、不追（生存门），落防御保命
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 5, "B", true, false, false, 60),
                        RobotE(4, 5, "C", true, false, false, 50)});
  f.self.hp = 40;
  f.score.resource_value = 120;
  f.score.effective_damage = 500;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.reason.find("集火参与") == std::string::npos, c);
  CHECK_C(c.action == "defend", c);
}

void Test80_DoomedTickStillShoots() {
  std::printf("[TEST] 80 必死局照打不白挨（R5-O 新增 4b）→ 20 HP + 两路火力，打人不 defend\n");
  CombatStrategy s;
  // hp=20、左右各 1 路直线枪线（2×20 ≥ 20 必倒，防御 2×10 ≥ 20 也救不了）
  // V8.3 实测 tick 29 是选了"低血避火 move down"白掉 20 血 → 现在先还一炮
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(6, 5, "B", true, false, false, 60),
                        RobotE(4, 5, "C", true, false, false, 50)});
  f.self.hp = 20;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.reason.find("必死还手") != std::string::npos, c);
}

void Test81_DoomedTickPrefersKillable() {
  std::printf("[TEST] 81 必死局先收头（R5-O 4b）→ 两侧都有目标时打血最少的\n");
  CombatStrategy s;
  // 同样必死；右 2 格 hp=100、左 2 格 hp=20 → 优先打左边收头（+60 击杀奖励）
  Perception f = Frame(10, "s", 5, 5,
                       {RobotE(7, 5, "B", true, false, false, 100),
                        RobotE(3, 5, "C", true, false, false, 20)});
  f.self.hp = 20;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "attack", c);
  CHECK_C(c.direction == "left", c);
}

void Test82_DefendStillBeatsDoomedShot() {
  std::printf("[TEST] 82 防御能活仍防御（R5-O 4b 不越界）→ 1 路火力防御 10 < HP 20\n");
  CombatStrategy s;
  Perception f = Frame(10, "s", 5, 5, {RobotE(6, 5, "B", true, false, false, 60)});
  f.self.hp = 20;
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.action == "defend", c);
}


// R5-P（10/31 用户："持续追击的 tick 要有限制，但能把对面压进角落就追到死"）
// 83：敌车持续拉开距离 → 距离停滞 4 tick 后放弃，不再陪跑（风筝免疫）
void Test83_PursuitStallAbandons() {
  std::printf("[TEST] 83 追不上就停（R5-P 停滞止损）→ 敌车持续拉开，第 6 tick 起不再追\n");
  CombatStrategy s;
  bool chased_first = false;
  bool chased_at_sixth = false;
  for (int k = 0; k < 7; ++k) {
    Perception f = Frame(1 + k, "s", 5, 5,
                         {RobotE(9 + k, 5, "B", true, false, false, 70)});
    f.score.resource_value = 120;  // V8.6：资源优先时远敌不主动追，先让资源饱和
    ActionChoice c = s.decide(f, nullptr);
    const bool chase = c.reason.find("单敌追猎") != std::string::npos;
    if (k == 0) chased_first = chase;
    if (k == 5) chased_at_sixth = chase;
    if (k == 0) CHECK_C(chase, c);
  }
  CHECK_C(chased_first, ActionChoice{});
  CHECK_C(!chased_at_sixth, ActionChoice{});
}

// 84：同一几何下，目标可逃格 ≤1（被封进死角）→ 不收追击预算限制，继续追
void Test84_CornerPinsChaseToDeath() {
  std::printf("[TEST] 84 压进死角就追到死（R5-Q）→ 可逃格 2 放弃 / 可逃格 1 继续\n");
  // 自由目标：可逃格 2 → 停滞 4 tick 后放弃
  CombatStrategy free_s;
  ActionChoice last_free;
  for (int k = 0; k < 8; ++k) {
    Perception f = Frame(1 + k, "s", 14, 14,
                         {RobotE(17, 17, "B", true, false, false, 70)});
    f.score.resource_value = 120;  // V8.6：资源优先时远敌不主动追
    last_free = free_s.decide(f, nullptr);
  }
  CHECK_C(last_free.reason.find("单敌追猎") == std::string::npos, last_free);
  // 封角目标：(16,17) 有障碍 → 只剩 (17,16) 一个出口 → 一直追
  CombatStrategy pin_s;
  ActionChoice last_pin;
  for (int k = 0; k < 8; ++k) {
    Perception f = Frame(1 + k, "s", 14, 14,
                         {RobotE(17, 17, "B", true, false, false, 70),
                          ObstacleE(16, 17)});
    f.score.resource_value = 120;  // V8.6：资源优先时远敌不主动追
    last_pin = pin_s.decide(f, nullptr);
  }
  CHECK_C(last_pin.reason.find("单敌追猎") != std::string::npos, last_pin);
}

// 85：隔墙目标 → 按障碍感知 BFS 绕行，不朝墙撞
void Test85_PursuitRoutesAroundObstacle() {
  std::printf("[TEST] 85 绕障碍追击（R5-P BFS 距离）→ 正下方是墙，第一步走两侧\n");
  CombatStrategy s;
  Perception f = Frame(1, "s", 5, 5,
                       {RobotE(5, 9, "B", true, false, false, 70), ObstacleE(5, 6)});
  f.score.resource_value = 120;  // V8.6：资源优先时远敌不主动追
  ActionChoice c = s.decide(f, nullptr);
  CHECK_C(c.reason.find("单敌追猎") != std::string::npos, c);
  CHECK_C(c.direction == "left" || c.direction == "right", c);
}

// 86：幻觉补偿期间收到 E_OOB 回执 → 立即停补偿，不再逐 Tick 空转
// （R6-B，10/31 第二阶段 v90 实测 tick 57–63：攻击段无移动回执 → 事件结束后
//  仍反向发，贴边把"想往下走"翻成"往上走"，连续 7 Tick E_OOB）
void Test86_IllusionOobStopsCompensation() {
  std::printf("[TEST] 86 幻觉补偿撞出 E_OOB → 立即停止补偿（R6-B）\n");
  CombatStrategy s;
  // t1 正常帧（发出 right）；t2 回执反向 → 幻觉生效，本帧反向发 left
  Perception f1 = Frame(10, "s", 5, 5);
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "move", c1);
  Perception f2 = Frame(11, "s", 5, 5);
  f2.self.has_last_action = true;
  f2.self.last_tick = 10;
  f2.self.last_action = "move";
  f2.self.last_moved = true;
  f2.self.has_last_direction = true;
  f2.self.last_effective_direction = "left";  // 与发出方向相反
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction == "left", c2);  // 补偿中
  // t3：补偿后的 move 被裁判判 E_OOB（无方向回执）→ 补偿解除，按真实意图发
  Perception f3 = Frame(12, "s", 5, 5);
  f3.self.has_last_action = true;
  f3.self.last_tick = 11;
  f3.self.last_action = "move";
  f3.self.last_moved = false;
  f3.self.has_last_error = true;
  f3.self.last_error = "E_OOB";
  ActionChoice c3 = s.decide(f3, nullptr);
  CHECK_C(c3.action == "move", c3);
  CHECK_C(c3.direction == "right", c3);  // 不再反向
}

// 87：环上出现单格障碍 → BFS 绕行锁存，不得与上一格来回死循环（R6-C）
// （10/31 巡逻仿真：环格 30% 被切断时旧实现 471/500 步是反向、覆盖仅 73%）
void Test87_RingDetourNoPingPong() {
  std::printf("[TEST] 87 环上单格障碍 → 绕行锁存，不回退死循环（R6-C）\n");
  CombatStrategy s;
  // 沿 18×18 环（d=5）从 (5,5) 顺时针走到 (12,12)，把 patrol_idx_ 推到 15
  const std::pair<int, int> walk[] = {
      {5, 5},  {6, 5},  {7, 5},  {8, 5},  {9, 5},  {10, 5}, {11, 5}, {12, 5},
      {12, 6}, {12, 7}, {12, 8}, {12, 9}, {12, 10}, {12, 11}, {12, 12}};
  int tick = 100;
  for (const auto &c : walk) {
    Perception f = Frame(tick++, "s", c.first, c.second);
    (void)s.decide(f, nullptr);
  }
  // 在 (11,12) 放障碍：下一路点 (11,12) 不可达 → 跳到 (10,12)，必须向上绕
  Perception f1 = Frame(tick++, "s", 12, 12, {ObstacleE(11, 12)});
  ActionChoice c1 = s.decide(f1, nullptr);
  CHECK_C(c1.action == "move", c1);
  CHECK_C(c1.direction != "left", c1);  // 不能朝障碍走
  // 下一 Tick 在 (12,11)：锁存生效 → 继续 BFS 绕行，不能按环序退回 (12,12)
  Perception f2 = Frame(tick++, "s", 12, 11, {ObstacleE(11, 12)});
  ActionChoice c2 = s.decide(f2, nullptr);
  CHECK_C(c2.action == "move", c2);
  CHECK_C(c2.direction != "down", c2);  // 旧实现会 down 回 (12,12) 死循环
}

int main() {
  std::printf("==== CombatStrategy 规则层离线自检（93 组场景）====\n");
  Test01_EmptyPerception();
  Test02_DeadWaits();
  Test03_SelfProtectedNoAttack();
  Test04_AttackInRange();
  Test05_OutOfRangeNoAttack();
  Test06_DiagonalNoAttack();
  Test07_DefendingTargetNowAttacked();
  Test08_ProtectedTargetNoAttack();
  Test09_LegalActionSet();
  Test10_Idempotent();
  Test11_LlmNullSafe();
  Test12_NewSessionClears();
  Test13_IdleRoams();
  Test14_KillableFirst();
  Test15_SaturationStillFights();
  Test16_RetreatBreaksLine();
  Test17_DefendUnderFocus();
  Test18_RetreatAvoidsObstacle();
  Test19_MissionFirstWhenSaturated();
  Test20_WanderRotatesWhenBlocked();
  Test21_LowHpTrappedDefends();
  Test22_RetreatNoOscillation();
  Test23_LowHpNoFleeOffLine();
  Test24_LowHpKeepsKillOnly();
  Test25_SurvivableKillTaken();
  Test26_IsolatedTradeKept();
  Test27_AheadByShotAttacks();
  Test28_RetreatHealRoute();
  Test29_NoHealWhenHealthy();
  Test30_ExpiredPlanNoHeal();
  Test31_HealWhenNoEnemyVisible();
  Test32_BadLlmContentNoCrash();
  Test33_AheadByShotEvenAfterRetaliation();
  Test35_NoClusterFleeWhenMissionPending();
  Test36_SingleEnemyNoClusterAvoid();
  Test37_EqualHpTradesNow();
  Test38_HuntSingleEnemyWhenHealthy();
  Test39_NoHuntWhenNotHealthy();
  Test40_CompletedMissionNotRevisited();
  Test41_RingPatrolFollowsRing();
  Test42_RingPatrolSkipsBlockedWaypoint();
  Test43_RingPatrolNoNearestSnap();
  Test44_RespawnReanchorsNearest();
  Test45_LowHpWinningTradeAttacks();
  Test46_LowHpIdleKeepsScoring();
  Test47_ClusterFocusLowestHp();
  Test47b_ResourcesExhaustedAfterLapUnlocksEngage();
  Test48_NoEngageWhenResourceLeft();
  Test48c_RememberedResourceKeepsEngageOff();
  Test49_DamageMissionUnlocksEngage();
  Test50_DefendingLowestHpSelected();
  Test51_ProtectedTargetSkipped();
  Test52_LowHpNoClusterEngage();
  Test53_SaturatedStillFocuses();
  Test54_SurvivalGateBlocksPursuit();
  Test55_ThreatStepGateAtHp50();
  Test56_DiagonalAdjacentAlignsGunline();
  Test57_PaidLifeTradesAtLowHp();
  Test58_PaidLifeTakesLethalKill();
  Test59_UnpaidLifeStillDefends();
  Test60_PaidLifeNoFlee();
  Test61_IllusionFlipsMove();
  Test62_IllusionStableThenReleased();
  Test63_IllusionDoesNotFlipAttack();
  Test64_ObstacleForgottenWhenGone();
  Test65_PatrolAnchorOnlyOnArrival();
  Test66_NoEngageBeforeFullLap();
  Test67_LatchedEngageSurvivesResourceSighting();
  Test68_MostRingCoverageLatchesEngage();
  Test69_BelowCoverageThresholdNoEngage();
  Test70_HpDominanceEngagesDespiteClosedGate();
  Test71_RingFollowingAlongRing();
  Test72_TeleportRespawnReanchorsNearRing();
  Test73_HuntWheneverHpHigher();
  Test74_MultiEnemyHpEdgeFocus();
  Test75_MultiEnemyNoFlee();
  Test76_SaturatedCounterPunch();
  Test77_PatrolAvoidsEnemyRange();
  Test78_SaturatedSoloChase();
  Test79_SaturatedSurvivalGateHolds();
  Test80_DoomedTickStillShoots();
  Test81_DoomedTickPrefersKillable();
  Test82_DefendStillBeatsDoomedShot();
  Test83_PursuitStallAbandons();
  Test84_CornerPinsChaseToDeath();
  Test85_PursuitRoutesAroundObstacle();
  Test86_IllusionOobStopsCompensation();
  Test87_RingDetourNoPingPong();
  Test88_ResourcePrioritySuppressesHunt();
  Test89_ResourceStepUnderFireIfSurvivable();
  Test90_ResourceMissionKeepsExploring();
  Test91_SaturatedResourcesYieldToCombat();
  Test92_SaturatedStillChasesWithPendingMission();
  std::printf("==== 失败 %d 项 ====\n", g_fail);
  if (g_fail == 0) {
    std::printf("全部通过。\n");
  } else {
    std::printf("（若 CombatStrategy 尚未实现：占位 wait 下仅核心场景 4 与游走兜底"
                " 13 红灯 %d 项断言、其余 11 组即绿属预期 TDD 基线；"
                "见 docs/combat_design.md §5.5）\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
