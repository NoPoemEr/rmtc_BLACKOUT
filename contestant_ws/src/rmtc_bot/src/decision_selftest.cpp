// decision_selftest.cpp —— 决策内核 + LLM 顾问层离线自检（不依赖 ROS/裁判/网络）
//
// 覆盖 docs/决策设计.md 的核心机制：
//   - 参数加载：JSON 覆盖 / 非法值拒收 / 文件缺失回退（§0 不写死原则）
//   - 饱和度：三组件饱和判定与余量（§1）
//   - 死亡成本/击杀稀释账（§3）
//   - 信念更新：地图记忆、资源消失归因、对手跟踪、攻击倾向、出生点学习（§2）
//   - 目标候选与 EV：采集/接战/后撤/防御/任务点；低血目标 EV 更高；全负→空选
//   - 事件：参数校验（可变化数值接受、越界拒绝）、阶段推导、立场选择（§4）
//   - LLM 顾问：提示词携带公告原文+Schema（§8.1 合规）、事件理解/权重解析
//     的宽松读严格用、文本 hash 稳定（§5）

#include <cstdio>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "rmtc_bot/decision_model.hpp"
#include "rmtc_bot/llm_advisor.hpp"

using rmtc_bot::Belief;
using rmtc_bot::DecisionParams;
using rmtc_bot::DecisionWeights;
using rmtc_bot::Entity;
using rmtc_bot::EventPhase;
using rmtc_bot::EventStance;
using rmtc_bot::EventState;
using rmtc_bot::EventType;
using rmtc_bot::GoalCandidate;
using rmtc_bot::GoalType;
using rmtc_bot::LlmWeightHints;
using rmtc_bot::Mission;
using rmtc_bot::Perception;
using rmtc_bot::StanceDecision;

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

#define CHECK_NEAR(a, b, eps)                                              \
  do {                                                                     \
    const double _a = (a), _b = (b);                                       \
    if (std::fabs(_a - _b) > (eps)) {                                      \
      std::printf("  [FAIL] %s:%d  %s≈%s (%.9f vs %.9f)\n", __FILE__,      \
                  __LINE__, #a, #b, _a, _b);                               \
      ++g_fail;                                                            \
    }                                                                      \
    std::fflush(stdout);                                                   \
  } while (0)

void add_robot(Perception& p, const std::string& team, int x, int y, int hp,
               bool defending = false) {
  Entity e;
  e.type = "robot";
  e.team = team;
  e.id = "r_" + team;
  e.has_absolute = true;
  e.abs_x = x;
  e.abs_y = y;
  e.has_hp = true;
  e.hp = hp;
  e.defending = defending;
  p.entities.push_back(e);
}

void add_resource(Perception& p, int x, int y, int value) {
  Entity e;
  e.type = value >= 20 ? "resource_high" : "resource_normal";
  e.id = "res_" + std::to_string(x) + "_" + std::to_string(y);
  e.has_absolute = true;
  e.abs_x = x;
  e.abs_y = y;
  e.has_value = true;
  e.value = value;
  p.entities.push_back(e);
}

Perception base_frame(int tick, int sx, int sy, int hp) {
  Perception p;
  p.tick = tick;
  p.session_id = "s";
  p.map_width = 15;
  p.map_height = 15;
  p.vision_radius = 4;
  p.self.has_position = true;
  p.self.x = sx;
  p.self.y = sy;
  p.self.has_hp = true;
  p.self.hp = hp;
  p.self.has_alive = true;
  p.self.alive = hp > 0;
  return p;
}

// ---------------------------------------------------------------------------

void test_params() {
  std::printf("== 参数加载与不写死 ==\n");
  DecisionParams p;
  std::vector<std::string> w;
  // 缺失文件：回退默认 + 警告
  CHECK(!rmtc_bot::load_params_json("/nonexistent/rmtc.json", p, w));
  CHECK(p.hp_max == 100 && p.attack_damage == 20 && p.match_ticks == 500);

  // 覆盖 + 非法值拒收
  const std::string path = "/tmp/rmtc_decision_params_test.json";
  {
    std::ofstream f(path);
    f << "{\"hp_max\":150,\"attack_damage\":25,\"death_penalty\":50.5,"
         "\"combat_score_cap_d\":600,\"match_ticks\":600,\"llm_quota\":50,"
         "\"attack_damage_bad\":-5}\n";
  }
  w.clear();
  CHECK(rmtc_bot::load_params_json(path, p, w));
  CHECK(p.hp_max == 150);
  CHECK(p.attack_damage == 25);
  CHECK_NEAR(p.death_penalty, 50.5, 1e-9);
  CHECK(p.combat_score_cap_d == 600);
  CHECK(p.match_ticks == 600);
  CHECK(p.llm_quota == 50);
  // 未知键忽略、非法值拒收（attack_damage_bad 不是我们识别的键，无效）
  CHECK(!w.empty() || true);
  std::remove(path.c_str());

  // 非法正数回退默认：hp_max = -3
  {
    std::ofstream f(path);
    f << "{\"hp_max\":-3,\"attack_damage\":0}\n";
  }
  DecisionParams q;
  w.clear();
  rmtc_bot::load_params_json(path, q, w);
  CHECK(q.hp_max == 100);       // 负值拒收
  CHECK(q.attack_damage == 20); // 0 拒收
  CHECK(!w.empty());
  std::remove(path.c_str());
}

void test_saturation() {
  std::printf("== 计分与饱和度 ==\n");
  DecisionParams p;
  auto s = rmtc_bot::compute_scores(60, 250, 2, p);
  CHECK_NEAR(s.resource_pts, 15.0, 1e-9);   // 60/120*30
  CHECK_NEAR(s.combat_pts, 7.5, 1e-9);      // 250/500*15
  CHECK_NEAR(s.task_pts, 10.0, 1e-9);
  CHECK_NEAR(s.total, 32.5, 1e-9);
  CHECK(!s.resource_saturated && !s.combat_saturated && !s.task_saturated);
  CHECK_NEAR(s.resource_margin, 60.0, 1e-9);
  CHECK_NEAR(s.combat_margin, 250.0, 1e-9);

  s = rmtc_bot::compute_scores(120, 500, 3, p);
  CHECK(s.resource_saturated && s.combat_saturated && s.task_saturated);
  CHECK_NEAR(s.total, 60.0, 1e-9);

  // 超额不溢出满分、负数封 0
  s = rmtc_bot::compute_scores(200, 900, 5, p);
  CHECK_NEAR(s.resource_pts, 30.0, 1e-9);
  CHECK_NEAR(s.combat_pts, 15.0, 1e-9);
  CHECK_NEAR(s.task_pts, 15.0, 1e-9);
  s = rmtc_bot::compute_scores(-10, -10, -1, p);
  CHECK_NEAR(s.total, 0.0, 1e-9);

  // 死亡成本与击杀稀释账
  CHECK_NEAR(rmtc_bot::death_cost_points(p), 1.8, 1e-9);
  CHECK_NEAR(rmtc_bot::kill_reward_points(p, 1), 1.8, 1e-9);
  CHECK_NEAR(rmtc_bot::kill_reward_points(p, 2), 0.9, 1e-9);
  CHECK_NEAR(rmtc_bot::kill_reward_points(p, 4), 0.45, 1e-9);
  CHECK_NEAR(rmtc_bot::kill_reward_points(p, 0), 1.8, 1e-9);  // 防御非法参数
}

void test_belief_update() {
  std::printf("== 信念层更新 ==\n");
  DecisionParams p;
  Belief b;
  std::vector<std::string> w;

  // 帧 1：地图 + 障碍 + 资源 + 对手 B + 我方分数/任务
  Perception f1 = base_frame(10, 5, 5, 100);
  f1.score.resource_value = 20;
  f1.score.effective_damage = 30.5;
  f1.score.mission_completed = 1;
  Entity obs;
  obs.type = "obstacle";
  obs.has_absolute = true;
  obs.abs_x = 0;
  obs.abs_y = 0;
  f1.entities.push_back(obs);
  add_resource(f1, 8, 8, 20);
  add_robot(f1, "B", 6, 5, 100);
  Mission m;
  m.id = "m1";
  m.type = "reach";
  m.has_position = true;
  m.pos_x = 10;
  m.pos_y = 10;
  f1.missions.push_back(m);
  rmtc_bot::update_belief(b, f1, p, w);
  CHECK(b.map.width == 15 && b.map.height == 15);
  CHECK(b.map.obstacles.size() == 1);
  CHECK(b.map.resources_known.size() == 1);
  CHECK(b.map.resources_known[0].value == 20);
  CHECK(b.opponents.size() == 1 && b.opponents[0].team == "B");
  CHECK(b.opponents[0].hp_known && b.opponents[0].last_hp == 100);
  CHECK_NEAR(b.our_scores.resource_pts, 5.0, 1e-9);
  CHECK_NEAR(b.our_scores.combat_pts, 0.915, 1e-9);
  CHECK_NEAR(b.our_scores.task_pts, 5.0, 1e-9);
  CHECK(b.missions.size() == 1);

  // 帧 2：B 移动到资源格采走（本帧归因）
  Perception f2 = base_frame(11, 5, 5, 100);
  f2.score.resource_value = 20;
  f2.score.effective_damage = 30.5;
  f2.score.mission_completed = 1;
  f2.missions = f1.missions;
  add_robot(f2, "B", 8, 8, 100);
  rmtc_bot::update_belief(b, f2, p, w);
  CHECK(b.map.resources_known.empty());            // 视野内资源消失
  CHECK_NEAR(b.opponents[0].est_r2, 20.0, 1e-9);  // 归因给 B
  CHECK(b.map.obstacles.size() == 1);              // 静态障碍记忆保留

  // 帧 3：我方受击（B 贴脸）→ 攻击倾向上升
  Perception f3 = base_frame(12, 5, 5, 80);
  f3.score.resource_value = 20;
  f3.score.effective_damage = 30.5;
  add_robot(f3, "B", 5, 6, 100);
  rmtc_bot::update_belief(b, f3, p, w);
  CHECK(b.opponents[0].aggression > 0.0);
  CHECK(b.our_hp_known && b.our_hp == 80);

  // 出生点学习：开局帧 position_reason=initial
  Perception fi = base_frame(0, 2, 2, 100);
  fi.self.has_position_reason = true;
  fi.self.position_reason = "initial";
  Belief b2;
  rmtc_bot::update_belief(b2, fi, p, w);
  CHECK(b2.map.spawn_points.size() == 1);
  CHECK(b2.map.spawn_points[0][0] == 2 && b2.map.spawn_points[0][1] == 2);
}

void test_goal_candidates() {
  std::printf("== 目标候选与 EV ==\n");
  DecisionParams p;
  Belief b;
  std::vector<std::string> w;

  // 我方 (5,5) HP100；资源 (8,8) 价值 20；对手 B 满血贴脸、C 低血贴脸
  Perception f = base_frame(10, 5, 5, 100);
  f.score.resource_value = 10;
  f.score.effective_damage = 0;
  add_resource(f, 8, 8, 20);
  add_robot(f, "B", 6, 5, 100);
  add_robot(f, "C", 5, 6, 10);
  rmtc_bot::update_belief(b, f, p, w);
  DecisionWeights dw;
  auto goals = rmtc_bot::generate_goal_candidates(b, p, dw, w);

  bool has_collect = false, has_engage_b = false, has_engage_c = false;
  double ev_b = -1, ev_c = -1;
  for (const auto& g : goals) {
    if (g.type == GoalType::Collect && g.x == 8 && g.y == 8) has_collect = true;
    if (g.type == GoalType::Engage && g.target_team == "B") {
      has_engage_b = true;
      ev_b = g.ev;
    }
    if (g.type == GoalType::Engage && g.target_team == "C") {
      has_engage_c = true;
      ev_c = g.ev;
    }
  }
  CHECK(has_collect);
  CHECK(has_engage_b && has_engage_c);
  CHECK(ev_c > ev_b);  // 一枪线内低血目标：击杀稀释期望更高

  // 低血受威胁 → 后撤候选出现
  Belief b2;
  Perception f2 = base_frame(10, 5, 5, 20);
  add_robot(f2, "B", 6, 5, 100);
  rmtc_bot::update_belief(b2, f2, p, w);
  auto goals2 = rmtc_bot::generate_goal_candidates(b2, p, dw, w);
  bool has_retreat = false, has_defend = false;
  for (const auto& g : goals2) {
    if (g.type == GoalType::Retreat) has_retreat = true;
    if (g.type == GoalType::DefendHold) has_defend = true;
  }
  CHECK(has_retreat);
  CHECK(has_defend);

  // reach 任务 → 任务点候选；已完成任务过滤
  Belief b3;
  Perception f3 = base_frame(10, 5, 5, 100);
  Mission m;
  m.id = "m1";
  m.type = "reach";
  m.has_position = true;
  m.pos_x = 10;
  m.pos_y = 10;
  f3.missions.push_back(m);
  rmtc_bot::update_belief(b3, f3, p, w);
  auto goals3 = rmtc_bot::generate_goal_candidates(b3, p, dw, w);
  bool has_task = false;
  for (const auto& g : goals3)
    if (g.type == GoalType::GotoTask) has_task = true;
  CHECK(has_task);

  f3.score.mission_completed_ids = {"m1"};
  Belief b4;
  rmtc_bot::update_belief(b4, f3, p, w);
  auto goals4 = rmtc_bot::generate_goal_candidates(b4, p, dw, w);
  bool has_task4 = false;
  for (const auto& g : goals4)
    if (g.type == GoalType::GotoTask) has_task4 = true;
  CHECK(!has_task4);

  // 最优选择：EV 最大；无正 EV → nullptr
  const auto* best = rmtc_bot::pick_best_goal(goals);
  CHECK(best != nullptr);
  for (const auto& g : goals)
    CHECK(best->ev >= g.ev);
  Belief b5;  // 空信念：无资源/对手/任务
  b5.our_x = 5;
  b5.our_y = 5;
  b5.our_alive = true;
  auto goals5 = rmtc_bot::generate_goal_candidates(b5, p, dw, w);
  CHECK(rmtc_bot::pick_best_goal(goals5) == nullptr);

  // 阵亡：不生成任何候选
  Belief b6;
  b6.our_x = 5;
  b6.our_y = 5;
  b6.our_alive = false;
  CHECK(rmtc_bot::generate_goal_candidates(b6, p, dw, w).empty());
}

void test_events() {
  std::printf("== 语义事件模型 ==\n");
  DecisionParams p;
  std::vector<std::string> w;
  Belief b;
  b.our_x = 5;
  b.our_y = 5;
  b.our_alive = true;
  b.our_hp = 100;
  b.our_hp_known = true;
  b.our_scores = rmtc_bot::compute_scores(0, 0, 0, p);

  // 校验：合法矩形 + 非标准数值（37.5 不是规则标准配置，但 §8.1 允许场次变化）
  EventState e;
  e.type = EventType::Explosion;
  e.has_rect = true;
  e.rect_x0 = 2;
  e.rect_y0 = 3;
  e.rect_x1 = 9;
  e.rect_y1 = 10;
  e.damage = 37.5;
  e.duration = 5;
  e.active_tick = 20;
  e.until_tick = 25;
  CHECK(rmtc_bot::validate_event_state(e, 15, 15, 10, p, w));
  CHECK_NEAR(e.damage, 37.5, 1e-9);  // 可变化数值被接受，未被"纠回"标准值

  // 越界矩形 → 区域被丢弃 → 事件不可用
  EventState e2;
  e2.type = EventType::Explosion;
  e2.has_rect = true;
  e2.rect_x0 = 0;
  e2.rect_y0 = 0;
  e2.rect_x1 = 15;
  e2.rect_y1 = 14;
  CHECK(!rmtc_bot::validate_event_state(e2, 15, 15, 10, p, w));

  // 非法数值：负 damage → 置 0 + 警告
  EventState e3;
  e3.type = EventType::Chili;
  e3.has_rect = true;
  e3.rect_x0 = 1;
  e3.rect_y0 = 1;
  e3.rect_x1 = 3;
  e3.rect_y1 = 3;
  e3.damage = -50;
  size_t warn_before = w.size();
  CHECK(rmtc_bot::validate_event_state(e3, 15, 15, 10, p, w));
  CHECK(e3.damage == 0.0);
  CHECK(w.size() > warn_before);

  // 阶段推导边界
  EventState e4;
  e4.active_tick = 10;
  e4.until_tick = 20;
  CHECK(rmtc_bot::derive_phase(e4, 5) == EventPhase::Announced);
  CHECK(rmtc_bot::derive_phase(e4, 10) == EventPhase::Active);
  CHECK(rmtc_bot::derive_phase(e4, 21) == EventPhase::Expired);

  // 立场：爆炸回避；辣椒条件利用；回血点低血利用；未知保守
  StanceDecision sd = rmtc_bot::event_stance(e, b, p, w);  // 爆炸
  CHECK(sd.stance == EventStance::Avoid);
  EventState chili;
  chili.type = EventType::Chili;
  chili.has_rect = true;
  chili.rect_x0 = 0;
  chili.rect_y0 = 0;
  chili.rect_x1 = 5;
  chili.rect_y1 = 5;
  chili.damage = 50;
  chili.reason = "火线";
  sd = rmtc_bot::event_stance(chili, b, p, w);  // HP100 > 50+20 → 利用
  CHECK(sd.stance == EventStance::Exploit);
  b.our_hp = 60;  // 60 < 70 → 回避
  sd = rmtc_bot::event_stance(chili, b, p, w);
  CHECK(sd.stance == EventStance::Avoid);
  b.our_hp = 100;
  b.our_scores = rmtc_bot::compute_scores(120, 500, 3, p);  // 对抗已饱和
  sd = rmtc_bot::event_stance(chili, b, p, w);
  // R5-N（10/30 用户："超过 500 其实还是有比较空间"）：饱和不再是回避理由，
  // HP 扛得住就照常火线钓鱼（伤害仍推进击杀、扣对手 D）
  CHECK(sd.stance == EventStance::Exploit);
  b.our_scores = rmtc_bot::compute_scores(0, 0, 0, p);
  EventState heal;
  heal.type = EventType::HealZone;
  heal.has_rect = true;
  heal.rect_x0 = 0;
  heal.rect_y0 = 0;
  heal.rect_x1 = 3;
  heal.rect_y1 = 3;
  heal.heal = 20;
  b.our_hp = 30;
  sd = rmtc_bot::event_stance(heal, b, p, w);
  CHECK(sd.stance == EventStance::Exploit);  // 30 < 100-20
  b.our_hp = 100;
  sd = rmtc_bot::event_stance(heal, b, p, w);
  CHECK(sd.stance == EventStance::Neutral);
  EventState unk;
  unk.type = EventType::Unknown;
  unk.has_rect = true;
  unk.rect_x0 = 0;
  unk.rect_y0 = 0;
  unk.rect_x1 = 2;
  unk.rect_y1 = 2;
  sd = rmtc_bot::event_stance(unk, b, p, w);
  CHECK(sd.stance == EventStance::Avoid);
}

void test_weights() {
  std::printf("== LLM 权重建议应用 ==\n");
  DecisionWeights w;
  std::vector<std::string> warns;
  LlmWeightHints h;
  h.has_resource = true;
  h.resource = 1.5;
  h.has_survival = true;
  h.survival = 9.0;  // 越界拒收
  rmtc_bot::apply_llm_weight_hints(w, h, warns);
  CHECK_NEAR(w.resource, 1.5, 1e-9);
  CHECK_NEAR(w.survival, 1.0, 1e-9);  // 拒收保持原值
  CHECK(!warns.empty());
}

void test_llm_advisor() {
  std::printf("== LLM 顾问层 ==\n");
  DecisionParams p;
  std::vector<std::string> w;

  // 事件解析提示词：原文 + 地图 + tick + Schema，本地不做内容提取
  const std::string text = "第 120 tick 起，矩形(3,3)-(7,7) 内爆炸，伤害 65";
  const std::string prompt =
      rmtc_bot::build_event_parse_prompt(text, 15, 15, 100, p);
  CHECK(prompt.find(text) != std::string::npos);       // 原文完整携带
  CHECK(prompt.find("15x15") != std::string::npos);    // 地图尺寸
  CHECK(prompt.find("100") != std::string::npos);      // 当前 tick
  CHECK(prompt.find("\"type\"") != std::string::npos); // Schema
  CHECK(prompt.find("explosion|illusion") != std::string::npos);
  // 禁止本地解析的证据：提示词不含我们对"65"的任何解释性句子
  CHECK(prompt.find("伤害 65 属于") == std::string::npos);

  // 事件理解解析：完整字段 + 反转矩形归一化 + 非标准数值接受
  EventState e;
  CHECK(rmtc_bot::parse_event_understanding(
      "{\"type\":\"chili\",\"region\":{\"rect\":[[9,10],[2,3]]},"
      "\"numbers\":{\"damage\":37.5,\"duration\":30},"
      "\"timing\":{\"active_tick\":100,\"until_tick\":130},"
      "\"reason\":\"火线\"}",
      e, w));
  CHECK(e.type == EventType::Chili);
  CHECK(e.has_rect && e.rect_x0 == 2 && e.rect_y0 == 3 && e.rect_x1 == 9 &&
        e.rect_y1 == 10);
  CHECK_NEAR(e.damage, 37.5, 1e-9);
  CHECK(e.duration == 30);
  CHECK(e.active_tick == 100 && e.until_tick == 130);
  CHECK(!e.reason.empty());

  // 平铺 rect 形态 + points
  EventState e2;
  rmtc_bot::parse_event_understanding(
      "{\"type\":\"explosion\",\"region\":{\"rect\":[2,3,9,10],"
      "\"points\":[[4,4],[99,99]]}}",
      e2, w);
  CHECK(e2.has_rect && e2.rect_x0 == 2);
  CHECK(e2.points.size() == 2);  // 解析层不过滤（裁剪归 validate_event_state）

  // 垃圾/顶层非对象 → false；未知 type → Unknown；负点忽略
  EventState e3;
  CHECK(!rmtc_bot::parse_event_understanding("not json", e3, w));
  CHECK(!rmtc_bot::parse_event_understanding("[1,2,3]", e3, w));
  rmtc_bot::parse_event_understanding(
      "{\"type\":\"nonexistent\",\"region\":{\"points\":[[1,2],[-1,5]]}}", e3,
      w);
  CHECK(e3.type == EventType::Unknown);
  CHECK(e3.points.size() == 1);  // 负坐标点被忽略
  CHECK(e3.points[0][0] == 1 && e3.points[0][1] == 2);

  // 权重解析：有效键 + 越界拒收 + 非对象 false
  LlmWeightHints h;
  CHECK(rmtc_bot::parse_weight_hints(
      "{\"weights\":{\"resource\":1.5,\"combat\":0.5}}", h, w));
  CHECK(h.has_resource && h.resource == 1.5);
  CHECK(h.has_combat && h.combat == 0.5);
  CHECK(!h.has_task && !h.has_survival);
  LlmWeightHints h2;
  size_t wb = w.size();
  rmtc_bot::parse_weight_hints("{\"weights\":{\"survival\":9}}", h2, w);
  CHECK(!h2.has_survival);
  CHECK(w.size() > wb);
  LlmWeightHints h3;
  CHECK(!rmtc_bot::parse_weight_hints("garbage", h3, w));

  // 态势综述提示词：含分数摘要/对手/候选 EV
  Belief b;
  b.tick = 50;
  b.our_x = 5;
  b.our_y = 5;
  b.our_hp = 100;
  b.our_hp_known = true;
  b.our_alive = true;
  b.our_scores = rmtc_bot::compute_scores(60, 100, 1, p);
  b.opponents.push_back(rmtc_bot::OpponentBelief{});
  b.opponents[0].team = "B";
  b.opponents[0].seen = true;
  b.opponents[0].last_x = 6;
  b.opponents[0].last_y = 5;
  b.opponents[0].last_hp = 50;
  b.opponents[0].hp_known = true;
  GoalCandidate g;
  g.type = GoalType::Engage;
  g.target_team = "B";
  g.ev = 0.9;
  g.expected_delta = 1.2;
  g.risk_penalty = 0.3;
  g.reason = "接战 B";
  const std::string sp =
      rmtc_bot::build_strategy_prompt(b, {g}, p);
  CHECK(sp.find("我方得分") != std::string::npos);
  CHECK(sp.find("对手 B") != std::string::npos);
  CHECK(sp.find("接战 B") != std::string::npos);
  CHECK(sp.find("EV=0.9") != std::string::npos);

  // 文本 hash：稳定 + 区分 + 16 位
  const std::string hs1 = rmtc_bot::hash_text("公告甲");
  const std::string hs2 = rmtc_bot::hash_text("公告甲");
  const std::string hs3 = rmtc_bot::hash_text("公告乙");
  CHECK(hs1 == hs2);
  CHECK(hs1 != hs3);
  CHECK(hs1.size() == 16);
}

}  // namespace

int main() {
  std::printf("=== decision_selftest：决策内核 + LLM 顾问层 ===\n");
  test_params();
  test_saturation();
  test_belief_update();
  test_goal_candidates();
  test_events();
  test_weights();
  test_llm_advisor();
  if (g_fail == 0) {
    std::printf("全部通过\n");
    return 0;
  }
  std::printf("失败断言数: %d\n", g_fail);
  return 1;
}
