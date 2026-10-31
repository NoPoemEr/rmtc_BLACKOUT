// decision_model.cpp —— 决策内核实现（纯函数，无 ROS/网络/LLM 依赖）
//
// 数值全部来自 DecisionParams（可被开场规则快照覆盖）；语义事件模型只由
// LLM 结构化输出构建（llm_advisor 解析），本文件绝不触碰 event_text 内容。

#include "rmtc_bot/decision_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

namespace rmtc_bot {

namespace {

// 参数防御：非正/非法的关键字段回退标准配置，防止除零与回绕。
DecisionParams safe_params(const DecisionParams& p) {
  DecisionParams q = p;
  if (q.hp_max <= 0) q.hp_max = 100;
  if (q.attack_damage <= 0) q.attack_damage = 20;
  if (q.attack_damage_defended <= 0) q.attack_damage_defended = 10;
  if (!(q.death_penalty > 0.0) || !std::isfinite(q.death_penalty))
    q.death_penalty = 60.0;
  if (!(q.kill_reward > 0.0) || !std::isfinite(q.kill_reward))
    q.kill_reward = 60.0;
  if (!(q.resource_score_cap_value > 0.0) ||
      !std::isfinite(q.resource_score_cap_value))
    q.resource_score_cap_value = 120.0;
  if (!(q.resource_score_max > 0.0) || !std::isfinite(q.resource_score_max))
    q.resource_score_max = 30.0;
  if (!(q.combat_score_cap_d > 0.0) || !std::isfinite(q.combat_score_cap_d))
    q.combat_score_cap_d = 500.0;
  if (!(q.combat_score_max > 0.0) || !std::isfinite(q.combat_score_max))
    q.combat_score_max = 15.0;
  if (!(q.task_score_each > 0.0) || !std::isfinite(q.task_score_each))
    q.task_score_each = 5.0;
  if (q.task_count_max <= 0) q.task_count_max = 3;
  if (q.match_ticks <= 0) q.match_ticks = 500;
  if (q.llm_quota <= 0) q.llm_quota = 100;
  return q;
}

void add_warning(std::vector<std::string>& w, const std::string& s) {
  if (w.size() < 64) w.push_back(s);  // 防警告洪水
}

// 读取有界正 int：类型不对/越界/非正 → 保留默认 + 警告
int read_int(const nlohmann::json& j, const char* key, int dflt, int lo, int hi,
             std::vector<std::string>& w, const std::string& where) {
  auto it = j.find(key);
  if (it == j.end()) return dflt;
  if (!it->is_number_integer()) {
    add_warning(w, where + ": " + key + " 非整数，保留默认");
    return dflt;
  }
  long long v = it->get<long long>();
  if (v < lo || v > hi) {
    add_warning(w, where + ": " + key + " 越界，保留默认");
    return dflt;
  }
  return static_cast<int>(v);
}

double read_double(const nlohmann::json& j, const char* key, double dflt,
                   double lo, double hi, std::vector<std::string>& w,
                   const std::string& where) {
  auto it = j.find(key);
  if (it == j.end()) return dflt;
  if (!it->is_number()) {
    add_warning(w, where + ": " + key + " 非数值，保留默认");
    return dflt;
  }
  double v = it->get<double>();
  if (!std::isfinite(v) || v < lo || v > hi) {
    add_warning(w, where + ": " + key + " 越界，保留默认");
    return dflt;
  }
  return v;
}

double manhattan(int x0, int y0, int x1, int y1) {
  return std::abs(x0 - x1) + std::abs(y0 - y1);
}

int chebyshev(int x0, int y0, int x1, int y1) {
  return std::max(std::abs(x0 - x1), std::abs(y0 - y1));
}

}  // namespace

// ---------------------------------------------------------------------------
// 参数加载
// ---------------------------------------------------------------------------

bool load_params_json(const std::string& path, DecisionParams& out,
                      std::vector<std::string>& warnings) {
  std::ifstream f(path);
  if (!f.good()) {
    add_warning(warnings, "参数文件不可读: " + path + "，沿用默认");
    return false;
  }
  nlohmann::json j;
  try {
    f >> j;
  } catch (const std::exception&) {
    add_warning(warnings, "参数文件 JSON 解析失败: " + path + "，沿用默认");
    return false;
  }
  if (!j.is_object()) {
    add_warning(warnings, "参数文件顶层非对象: " + path + "，沿用默认");
    return false;
  }
  const std::string where = "params";
  out.hp_max = read_int(j, "hp_max", out.hp_max, 1, 1000000, warnings, where);
  out.hp_max = read_int(j, "hpMax", out.hp_max, 1, 1000000, warnings, where);
  out.attack_damage = read_int(j, "attack_damage", out.attack_damage, 1,
                               1000000, warnings, where);
  out.attack_damage_defended =
      read_int(j, "attack_damage_defended", out.attack_damage_defended, 1,
               1000000, warnings, where);
  out.death_penalty = read_double(j, "death_penalty", out.death_penalty, 0.0,
                                  1e6, warnings, where);
  out.kill_reward = read_double(j, "kill_reward", out.kill_reward, 0.0, 1e6,
                                warnings, where);
  out.resource_score_cap_value =
      read_double(j, "resource_score_cap_value", out.resource_score_cap_value,
                  1e-9, 1e6, warnings, where);
  out.resource_score_max = read_double(j, "resource_score_max",
                                       out.resource_score_max, 0.0, 1e6,
                                       warnings, where);
  out.combat_score_cap_d = read_double(j, "combat_score_cap_d",
                                       out.combat_score_cap_d, 1e-9, 1e6,
                                       warnings, where);
  out.combat_score_max = read_double(j, "combat_score_max",
                                     out.combat_score_max, 0.0, 1e6, warnings,
                                     where);
  out.task_score_each = read_double(j, "task_score_each", out.task_score_each,
                                    0.0, 1e6, warnings, where);
  out.task_count_max = read_int(j, "task_count_max", out.task_count_max, 1, 100,
                                warnings, where);
  out.respawn_wait_ticks = read_int(j, "respawn_wait_ticks",
                                    out.respawn_wait_ticks, 0, 1000000,
                                    warnings, where);
  out.spawn_protect_ticks = read_int(j, "spawn_protect_ticks",
                                     out.spawn_protect_ticks, 0, 1000000,
                                     warnings, where);
  out.match_ticks =
      read_int(j, "match_ticks", out.match_ticks, 1, 10000000, warnings, where);
  out.llm_quota = read_int(j, "llm_quota", out.llm_quota, 1, 10000000, warnings,
                           where);
  out.patrol_lap_coverage =
      read_double(j, "patrol_lap_coverage", out.patrol_lap_coverage, 0.05, 1.0,
                  warnings, where);
  // R5-P 追击纪律（10/31）
  out.pursue_max_ticks =
      read_int(j, "pursue_max_ticks", out.pursue_max_ticks, 1, 500, warnings, where);
  out.pursue_max_stall =
      read_int(j, "pursue_max_stall", out.pursue_max_stall, 1, 500, warnings, where);
  out.pursue_cooldown =
      read_int(j, "pursue_cooldown", out.pursue_cooldown, 0, 500, warnings, where);
  out.pursue_corner_escape = read_int(j, "pursue_corner_escape",
                                      out.pursue_corner_escape, 0, 4, warnings, where);
  return true;
}

// ---------------------------------------------------------------------------
// 计分与饱和度
// ---------------------------------------------------------------------------

ScoreComponents compute_scores(double r2, double d, int tasks_done,
                               const DecisionParams& p) {
  const DecisionParams q = safe_params(p);
  ScoreComponents s;
  const double r2c = std::max(0.0, r2);
  const double dc = std::max(0.0, d);
  s.resource_pts = q.resource_score_max *
                   std::min(r2c / q.resource_score_cap_value, 1.0);
  s.combat_pts =
      q.combat_score_max * std::min(dc / q.combat_score_cap_d, 1.0);
  const int m = std::max(0, std::min(tasks_done, q.task_count_max));
  s.task_pts = q.task_score_each * static_cast<double>(m);
  s.total = s.resource_pts + s.combat_pts + s.task_pts;
  s.resource_saturated = r2c >= q.resource_score_cap_value;
  s.combat_saturated = dc >= q.combat_score_cap_d;
  s.task_saturated = m >= q.task_count_max;
  s.resource_margin = std::max(0.0, q.resource_score_cap_value - r2c);
  s.combat_margin = std::max(0.0, q.combat_score_cap_d - dc);
  return s;
}

double death_cost_points(const DecisionParams& p) {
  const DecisionParams q = safe_params(p);
  return q.death_penalty / q.combat_score_cap_d * q.combat_score_max;
}

double kill_reward_points(const DecisionParams& p, int contributors) {
  const DecisionParams q = safe_params(p);
  const int n = std::max(1, contributors);
  return q.kill_reward / q.combat_score_cap_d * q.combat_score_max /
         static_cast<double>(n);
}

// ---------------------------------------------------------------------------
// 语义事件模型
// ---------------------------------------------------------------------------

EventPhase derive_phase(const EventState& e, int current_tick) {
  if (e.until_tick >= 0 && current_tick > e.until_tick) return EventPhase::Expired;
  if (e.active_tick >= 0 && current_tick >= e.active_tick) return EventPhase::Active;
  return EventPhase::Announced;
}

bool validate_event_state(EventState& e, int map_w, int map_h, int current_tick,
                          const DecisionParams& p,
                          std::vector<std::string>& warnings) {
  const DecisionParams q = safe_params(p);
  bool usable = e.type != EventType::Unknown;

  // 区域校验：地图已知时裁剪；全越界 → 事件不可用
  if (map_w > 0 && map_h > 0) {
    if (e.has_rect) {
      const bool ok = e.rect_x0 >= 0 && e.rect_y0 >= 0 && e.rect_x1 < map_w &&
                      e.rect_y1 < map_h && e.rect_x0 <= e.rect_x1 &&
                      e.rect_y0 <= e.rect_y1;
      if (!ok) {
        add_warning(warnings, "event: rect 越界/逆序，丢弃区域");
        e.has_rect = false;
      }
    }
    std::vector<std::array<int, 2>> kept;
    for (const auto& pt : e.points) {
      if (pt[0] >= 0 && pt[1] >= 0 && pt[0] < map_w && pt[1] < map_h)
        kept.push_back(pt);
      else
        add_warning(warnings, "event: 越界点剔除");
    }
    e.points = kept;
  } else {
    add_warning(warnings, "event: 地图未知，跳过区域校验");
  }
  if (!e.has_rect && e.points.empty()) {
    usable = false;
    add_warning(warnings, "event: 无有效区域表达，事件不可用");
  }

  // 时间窗校验（数值可变化，但必须在场次范围内、语义一致）
  if (e.active_tick > q.match_ticks) {
    add_warning(warnings, "event: active_tick 超出场次，降级为未知");
    e.active_tick = -1;
  }
  if (e.until_tick < 0 && e.duration > 0 && e.active_tick >= 0) {
    e.until_tick = std::min(e.active_tick + e.duration - 1, q.match_ticks);
  }
  if (e.until_tick > q.match_ticks) {
    add_warning(warnings, "event: until_tick 超出场次，裁剪到场末");
    e.until_tick = q.match_ticks;
  }
  if (e.active_tick >= 0 && e.until_tick >= 0 && e.until_tick < e.active_tick) {
    add_warning(warnings, "event: 结束早于生效，时间窗作废");
    e.until_tick = -1;
  }
  if (current_tick >= 0 && e.until_tick >= 0 && current_tick > e.until_tick) {
    // 已过期事件保留作历史，但不再用于规划
    usable = false;
  }

  // 数值域校验：有限、非负、宽松上限（不要求等于标准配置——§8.1 明示可变化）
  const auto check_num = [&](double& v, const char* name) {
    if (!std::isfinite(v) || v < 0.0 || v > 1e5) {
      add_warning(warnings, std::string("event: ") + name + " 非法，置 0");
      v = 0.0;
    }
  };
  check_num(e.damage, "damage");
  check_num(e.heal, "heal");
  if (e.duration < 0 || e.duration > q.match_ticks) {
    add_warning(warnings, "event: duration 非法，置 0");
    e.duration = 0;
  }
  return usable;
}

// ---------------------------------------------------------------------------
// 信念层
// ---------------------------------------------------------------------------

void update_belief(Belief& b, const Perception& p, const DecisionParams& params,
                   std::vector<std::string>& warnings) {
  const DecisionParams q = safe_params(params);
  b.tick = p.tick;

  // ---- 地图 ----
  if (p.map_width > 0 && p.map_height > 0) {
    if (b.map.width == 0 && b.map.height == 0) {
      b.map.width = p.map_width;
      b.map.height = p.map_height;
    } else if (b.map.width != p.map_width || b.map.height != p.map_height) {
      add_warning(warnings, "belief: 地图尺寸变化，忽略本帧");
    }
  }

  // ---- 我方状态镜像 ----
  if (p.self.has_position) {
    b.our_x = p.self.x;
    b.our_y = p.self.y;
  }
  if (p.self.has_hp) {
    b.our_hp = p.self.hp;
    b.our_hp_known = true;
  }
  if (p.self.has_alive) b.our_alive = p.self.alive;
  b.our_defending = p.self.defending;
  b.our_protected = p.self.protected_flag;
  b.our_scores = compute_scores(static_cast<double>(p.score.resource_value),
                                p.score.effective_damage,
                                p.score.mission_completed, q);
  b.missions = p.missions;
  b.completed_mission_ids = p.score.mission_completed_ids;
  b.mission_text = p.mission_text;
  b.event_text = p.event_text;

  // 出生点学习：开局/重生帧的本车位置
  if (p.self.has_position_reason &&
      (p.self.position_reason == "initial" || p.self.position_reason == "respawn")) {
    const std::array<int, 2> pt = {b.our_x, b.our_y};
    if (b.our_x >= 0 &&
        std::find(b.map.spawn_points.begin(), b.map.spawn_points.end(), pt) ==
            b.map.spawn_points.end())
      b.map.spawn_points.push_back(pt);
  }

  // ---- 本帧可见资源集合（绝对坐标） ----
  std::vector<std::array<int, 2>> visible_res;
  std::vector<MapMemory::Resource> visible_res_full;
  std::vector<std::array<int, 2>> visible_obstacles;  // R5-C 遗忘判定用
  for (const auto& en : p.entities) {
    if (en.type == "resource_normal" || en.type == "resource_high") {
      if (en.has_absolute) {
        visible_res.push_back({en.abs_x, en.abs_y});
        MapMemory::Resource r;
        r.x = en.abs_x;
        r.y = en.abs_y;
        r.value = en.has_value ? en.value : 0;
        visible_res_full.push_back(r);
      }
    } else if (en.type == "obstacle" && en.has_absolute) {
      const std::array<int, 2> pt = {en.abs_x, en.abs_y};
      if (std::find(b.map.obstacles.begin(), b.map.obstacles.end(), pt) ==
          b.map.obstacles.end())
        b.map.obstacles.push_back(pt);
      visible_obstacles.push_back(pt);
    }
  }

  // R5-C（10/30 V8 练习赛实测）：障碍记忆只增不减 → WorldSplit 临时十字墙
  // 消失后仍被永久记忆，BFS 长期绕"鬼墙"绕圈（tick 210–325、440–500 各
  // 60+ tick 在 (9,9)–(12,12) 空转徘徊）。视野内的记忆障碍本帧已不在实体
  // 列表 → 遗忘（临时墙消失即解除）；视野外不动（出视野 ≠ 消失）。
  {
    std::vector<std::array<int, 2>> kept;
    for (const auto& o : b.map.obstacles) {
      const bool in_vision =
          b.our_x >= 0 &&
          chebyshev(b.our_x, b.our_y, o[0], o[1]) <= p.vision_radius;
      const bool seen_now =
          std::find(visible_obstacles.begin(), visible_obstacles.end(), o) !=
          visible_obstacles.end();
      if (in_vision && !seen_now) continue;  // 遗忘
      kept.push_back(o);
    }
    b.map.obstacles = kept;
  }

  // ---- 资源消失归因：视野内消失 = 被采集 ----
  // 注意：实体缺席 ≠ 被采（视野外资源本来就不可见），只在"该格处于我方当前视野
  // 半径内"时才认定为被采集；归因到上一帧占据该格的对手（上一帧位置 == 该格）。
  std::vector<MapMemory::Resource> still_there;
  for (const auto& r : b.map.resources_known) {
    bool visible_now = false;
    for (const auto& v : visible_res)
      if (v[0] == r.x && v[1] == r.y) visible_now = true;
    if (visible_now) {
      still_there.push_back(r);
      continue;
    }
    bool in_vision_now = b.our_x >= 0 &&
                         chebyshev(b.our_x, b.our_y, r.x, r.y) <= p.vision_radius;
    if (!in_vision_now) {
      still_there.push_back(r);  // 只是出了视野，保留记忆
      continue;
    }
    // 视野内消失 → 被采。归因对手：本帧或上一帧位置 == 该格的对手采得
    // （本帧优先——对手移动到资源格当帧采走）；我方自己采的由感知
    // score.resource_value 反映，此处不重复归因。
    for (const auto& prev : b.prev_opp_pos) {
      if (prev[1] == r.x && prev[2] == r.y) {
        auto& ob = b.opponents[static_cast<size_t>(prev[0])];
        ob.est_r2 += static_cast<double>(std::max(0, r.value));
        ob.confidence = std::min(1.0, ob.confidence + 0.1);
      }
    }
    for (const auto& en : p.entities) {
      if (en.type != "robot" || !en.has_absolute) continue;
      if (en.abs_x == r.x && en.abs_y == r.y) {
        for (auto& ob : b.opponents) {
          if (ob.team == en.team) {
            ob.est_r2 += static_cast<double>(std::max(0, r.value));
            ob.confidence = std::min(1.0, ob.confidence + 0.1);
          }
        }
      }
    }
  }
  b.map.resources_known = still_there;
  // 新资源入记忆
  for (const auto& r : visible_res_full) {
    bool known = false;
    for (const auto& k : b.map.resources_known)
      if (k.x == r.x && k.y == r.y) known = true;
    if (!known) b.map.resources_known.push_back(r);
  }

  // ---- 对手跟踪 ----
  for (const auto& en : p.entities) {
    if (en.type != "robot" || en.team.empty()) continue;
    OpponentBelief* ob = nullptr;
    for (auto& o : b.opponents)
      if (o.team == en.team) ob = &o;
    if (ob == nullptr) {
      b.opponents.push_back(OpponentBelief{});
      ob = &b.opponents.back();
      ob->team = en.team;
    }
    ob->seen = true;
    ob->last_seen_tick = p.tick;
    if (en.has_absolute) {
      ob->last_x = en.abs_x;
      ob->last_y = en.abs_y;
    }
    if (en.has_hp) {
      ob->last_hp = en.hp;
      ob->hp_known = true;
    }
    if (en.defending) ++ob->defends_observed;
    ob->confidence = std::min(1.0, ob->confidence + 0.1);
    // 疑似出生点：满血 + 出生保护 + 我方此前未见
    if (en.protected_flag && en.has_hp && en.hp == q.hp_max &&
        en.has_absolute) {
      const std::array<int, 2> pt = {en.abs_x, en.abs_y};
      if (std::find(b.map.spawn_points.begin(), b.map.spawn_points.end(), pt) ==
          b.map.spawn_points.end())
        b.map.spawn_points.push_back(pt);
    }
  }
  // 未见对手的置信度衰减
  for (auto& o : b.opponents) {
    if (o.last_seen_tick != p.tick) o.confidence *= 0.97;
  }

  // ---- 攻击倾向估计：我方受击 + 该对手近场 → aggression 上升 ----
  const bool took_damage =
      b.prev_our_hp >= 0 && b.our_hp_known && b.our_hp < b.prev_our_hp;
  for (auto& o : b.opponents) {
    const bool near = took_damage && b.our_x >= 0 && o.last_x >= 0 &&
                      chebyshev(b.our_x, b.our_y, o.last_x, o.last_y) <= 2;
    if (near)
      o.aggression = std::min(1.0, o.aggression + 0.25);
    else
      o.aggression *= 0.95;
  }

  // ---- 跨帧快照 ----
  b.prev_opp_pos.clear();
  for (size_t i = 0; i < b.opponents.size(); ++i) {
    const auto& o = b.opponents[i];
    if (o.last_x >= 0)
      b.prev_opp_pos.push_back(
          {static_cast<int>(i), o.last_x, o.last_y});
  }
  b.prev_our_hp = b.our_hp_known ? b.our_hp : -1;
}

// ---------------------------------------------------------------------------
// 目标效用
// ---------------------------------------------------------------------------

void apply_llm_weight_hints(DecisionWeights& w, const LlmWeightHints& h,
                            std::vector<std::string>& warnings) {
  const double lo = 0.2, hi = 5.0;
  const auto apply_one = [&](bool has, double v, double& dst, const char* name) {
    if (!has) return;
    if (!std::isfinite(v) || v < lo || v > hi) {
      add_warning(warnings, std::string("llm weight: ") + name +
                                " 越界，拒收");
      return;
    }
    dst = std::min(hi, std::max(lo, dst * v));
  };
  apply_one(h.has_resource, h.resource, w.resource, "resource");
  apply_one(h.has_task, h.task, w.task, "task");
  apply_one(h.has_combat, h.combat, w.combat, "combat");
  apply_one(h.has_survival, h.survival, w.survival, "survival");
}

std::vector<GoalCandidate> generate_goal_candidates(
    const Belief& b, const DecisionParams& params, const DecisionWeights& w,
    std::vector<std::string>& warnings) {
  const DecisionParams q = safe_params(params);
  std::vector<GoalCandidate> goals;
  if (b.our_x < 0 || b.our_y < 0 || !b.our_alive) return goals;  // 无可规划

  // 伤害→分 换算
  const auto dmg_pts = [&](double dmg) {
    return dmg / q.combat_score_cap_d * q.combat_score_max;
  };

  // ---- Collect：已知资源 ----
  for (const auto& r : b.map.resources_known) {
    const double d = manhattan(b.our_x, b.our_y, r.x, r.y);
    const double rf = 1.0 / (1.0 + 0.1 * d);
    GoalCandidate g;
    g.type = GoalType::Collect;
    g.x = r.x;
    g.y = r.y;
    g.expected_delta =
        std::max(0, r.value) / q.resource_score_cap_value *
        q.resource_score_max * w.resource * rf;
    g.ev = g.expected_delta;
    g.reason = "采集资源 value=" + std::to_string(r.value);
    goals.push_back(g);
  }

  // ---- 近场对手统计（供 Engage/Retreat/DefendHold 共用） ----
  std::vector<const OpponentBelief*> nearby;
  for (const auto& o : b.opponents) {
    if (!o.seen || o.last_x < 0) continue;
    if (o.hp_known && o.last_hp <= 0) continue;  // 已阵亡
    if (chebyshev(b.our_x, b.our_y, o.last_x, o.last_y) <= 2)
      nearby.push_back(&o);
  }
  const int k = static_cast<int>(nearby.size());
  const double incoming = k * static_cast<double>(q.attack_damage);  // 近似

  // ---- Engage：逐对手 ----
  for (const auto& o : b.opponents) {
    if (!o.seen || o.last_x < 0) continue;
    if (o.hp_known && o.last_hp <= 0) continue;
    const double d = manhattan(b.our_x, b.our_y, o.last_x, o.last_y);
    // 期望伤害：射程内（直线 1–2 格，此处用曼哈顿近似）；防御标记只是"最近
    // 状态"，协议明示不保证，故不据此打折（保守取普攻伤害）。射程外按接近
    // 折扣估计（v0 启发式，待 1-tick 精确推演替换）。
    double expected_dmg = 0.0;
    if (d <= 2.0) {
      expected_dmg = static_cast<double>(q.attack_damage);
    } else {
      expected_dmg = static_cast<double>(q.attack_damage) * 0.5 /
                     (1.0 + (d - 2.0));
    }
    double kill_ev = 0.0;
    if (o.hp_known && o.last_hp <= q.attack_damage) {
      // 一枪线内：预期进入致死批次，按 2 人平分保守估计
      kill_ev = kill_reward_points(q, 2);
    }
    const double delta =
        (dmg_pts(expected_dmg) + kill_ev) * w.combat;
    // 死亡风险：近场敌人合计火力 vs 我方 HP（攻击当 tick 不防御）
    double p_die = 0.0;
    if (b.our_hp_known && incoming > 0.0) {
      p_die = std::min(0.9, 0.3 * incoming / static_cast<double>(b.our_hp));
    }
    const double risk = p_die * death_cost_points(q) * w.survival;
    GoalCandidate g;
    g.type = GoalType::Engage;
    g.target_team = o.team;
    g.x = o.last_x;
    g.y = o.last_y;
    g.expected_delta = delta;
    g.risk_penalty = risk;
    g.ev = delta - risk;
    g.reason = "接战 " + o.team + " hp=" +
               (o.hp_known ? std::to_string(o.last_hp) : "?");
    goals.push_back(g);
  }

  // ---- Retreat：低血受威胁 ----
  if (b.our_hp_known && k > 0 &&
      b.our_hp <= q.attack_damage * k) {
    GoalCandidate g;
    g.type = GoalType::Retreat;
    g.expected_delta = 0.0;
    // 留下的估计死亡概率（v0 启发式 0.6）→ 避免的期望死亡成本
    g.ev = death_cost_points(q) * w.survival * 0.6;
    g.reason = "后撤：低血且近场敌数=" + std::to_string(k);
    goals.push_back(g);
  }

  // ---- DefendHold：被集火时防御减半 ----
  if (b.our_hp_known && b.our_hp > 0 && k >= 1 &&
      incoming >= q.attack_damage) {
    GoalCandidate g;
    g.type = GoalType::DefendHold;
    g.x = b.our_x;
    g.y = b.our_y;
    g.expected_delta = dmg_pts(incoming) * 0.5 * w.survival;
    g.ev = g.expected_delta;
    g.reason = "防御抗集火：近场敌数=" + std::to_string(k);
    goals.push_back(g);
  }

  // ---- GotoTask：reach 类任务（已完成的不再生成） ----
  for (const auto& m : b.missions) {
    if (m.type != "reach" || !m.has_position) continue;
    bool done = false;
    for (const auto& id : b.completed_mission_ids)
      if (id == m.id) done = true;
    if (done) continue;
    const double d = manhattan(b.our_x, b.our_y, m.pos_x, m.pos_y);
    const double rf = 1.0 / (1.0 + 0.1 * d);
    GoalCandidate g;
    g.type = GoalType::GotoTask;
    g.x = m.pos_x;
    g.y = m.pos_y;
    g.expected_delta = q.task_score_each * w.task * rf;
    g.ev = g.expected_delta;
    g.reason = "前往任务点 (" + std::to_string(m.pos_x) + "," +
               std::to_string(m.pos_y) + ")";
    goals.push_back(g);
  }

  // ---- EventStance：可利用事件（辣椒钓鱼等，v0 简版） ----
  for (const auto& e : b.events) {
    const StanceDecision sd = event_stance(e, b, q, warnings);
    if (sd.stance != EventStance::Exploit) continue;
    GoalCandidate g;
    g.type = GoalType::EventStance;
    g.event_hash = e.source_hash;
    if (!e.points.empty()) {
      g.x = e.points[0][0];
      g.y = e.points[0][1];
    } else if (e.has_rect) {
      g.x = (e.rect_x0 + e.rect_x1) / 2;
      g.y = (e.rect_y0 + e.rect_y1) / 2;
    }
    // 期望：火线命中约 1 名敌人（v0 启发式）；风险：我方 HP 不足以吃下事件伤害
    double exploit_ev = dmg_pts(std::min(e.damage, q.combat_score_cap_d)) *
                        0.5 * w.combat;
    double risk = 0.0;
    if (b.our_hp_known &&
        static_cast<double>(b.our_hp) <= e.damage + q.attack_damage)
      risk = death_cost_points(q) * w.survival * 0.5;
    g.expected_delta = exploit_ev;
    g.risk_penalty = risk;
    g.ev = exploit_ev - risk;
    g.reason = "利用事件: " + e.reason;
    goals.push_back(g);
  }

  return goals;
}

const GoalCandidate* pick_best_goal(const std::vector<GoalCandidate>& goals) {
  const GoalCandidate* best = nullptr;
  for (const auto& g : goals) {
    if (g.ev <= 0.0) continue;
    if (best == nullptr || g.ev > best->ev) best = &g;
  }
  return best;
}

// ---------------------------------------------------------------------------
// 事件立场
// ---------------------------------------------------------------------------

StanceDecision event_stance(const EventState& e, const Belief& b,
                            const DecisionParams& p,
                            std::vector<std::string>& warnings) {
  (void)warnings;  // 立场判定暂不产生警告
  const DecisionParams q = safe_params(p);
  StanceDecision d;
  if (!b.our_alive) {
    d.stance = EventStance::Avoid;
    d.reason = "已阵亡，无可作为";
    return d;
  }
  switch (e.type) {
    case EventType::Explosion:
      d.stance = EventStance::Avoid;
      d.reason = "rect 型环境事件：回避，环境死亡不发奖励仍扣分";
      break;
    case EventType::WorldSplit:
      d.stance = EventStance::Avoid;
      d.reason = "cross 型环境事件：十字线即死即活仍扣分，回避";
      break;
    case EventType::Chili: {
      // 火线钓鱼：R5-N（10/30）起饱和度不再是理由——超 500 的伤害仍推进击杀、
      // 扣对手 60 D（相对排名有效），唯一门槛是"我方 HP 扛得住事件伤害+一次普攻"
      const bool hp_ok =
          b.our_hp_known &&
          static_cast<double>(b.our_hp) > e.damage + q.attack_damage;
      if (hp_ok) {
        d.stance = EventStance::Exploit;
        d.reason = "辣椒：火线钓鱼（交点免疫，规则允许主动踩点）";
      } else {
        d.stance = EventStance::Avoid;
        d.reason = "辣椒：HP 扛不住事件伤害，回避";
      }
      break;
    }
    case EventType::HealZone: {
      const bool need_heal =
          b.our_hp_known &&
          static_cast<double>(b.our_hp) < q.hp_max - e.heal;
      d.stance = need_heal ? EventStance::Exploit : EventStance::Neutral;
      d.reason = need_heal ? "回血点：血线不足，前往回复"
                           : "回血点：血线充足，无强立场";
      break;
    }
    case EventType::Thorns:
    case EventType::Illusion:
    case EventType::Vulnerable:
    case EventType::Fortress:
      d.stance = EventStance::Neutral;
      d.reason = "执行层参考事件表补偿（防御/方向反转/避免无谓移动）";
      break;
    case EventType::Unknown:
    case EventType::Other:
    default:
      d.stance = EventStance::Avoid;
      d.reason = "未识别事件：一律保守回避";
      break;
  }
  return d;
}

}  // namespace rmtc_bot
