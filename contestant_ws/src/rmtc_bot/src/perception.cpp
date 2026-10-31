#include "rmtc_bot/perception.hpp"

#include <nlohmann/json.hpp>

#include <climits>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rmtc_bot {

namespace {

// ---- 安全读取小工具：字段缺失/类型错时返回 false，绝不抛异常 ----

bool read_string(const nlohmann::json &j, const char *key, std::string &out) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return false;
  out = it->get<std::string>();
  return true;
}

// 有界整数读取（10/27 审查 H1 修复）：nlohmann 对超出 int 范围的整数
// is_number_integer() 仍返回 true，get<int>() 会静默按 32 位回绕。
// 这里显式按 int64/uint64 取值并做范围检查，越界一律视为非法（等同缺失）。
// tick 等关键字段越界 → parse_perception 返回 false → 本帧不用于动作回填。
bool bounded_int(const nlohmann::json &v, int &out) {
  if (!v.is_number_integer()) return false;
  if (v.is_number_unsigned()) {
    const std::uint64_t uv = v.get<std::uint64_t>();
    if (uv > static_cast<std::uint64_t>(INT_MAX)) return false;
    out = static_cast<int>(uv);
    return true;
  }
  const std::int64_t sv = v.get<std::int64_t>();
  if (sv < INT_MIN || sv > INT_MAX) return false;
  out = static_cast<int>(sv);
  return true;
}

bool read_int(const nlohmann::json &j, const char *key, int &out) {
  auto it = j.find(key);
  if (it == j.end()) return false;
  return bounded_int(*it, out);
}

// 数值字段（整数或小数都收）：effective_damage 在线上是浮点（规则允许小数，
// 如残血分摊 5.0），资源分等其它字段是整数——用同一个读取器兼容两种。
// 非有限值（inf/NaN）视为非法。
bool read_number(const nlohmann::json &j, const char *key, double &out) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return false;
  const double v = it->get<double>();
  if (!std::isfinite(v)) return false;
  out = v;
  return true;
}

bool read_bool(const nlohmann::json &j, const char *key, bool &out) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_boolean()) return false;
  out = it->get<bool>();
  return true;
}

// 读 [x,y] 整数坐标数组（两个元素都做有界检查）
bool read_position(const nlohmann::json &j, const char *key, int &x, int &y) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() != 2) return false;
  return bounded_int((*it)[0], x) && bounded_int((*it)[1], y);
}

}  // namespace

bool parse_relative_position(const std::string &grid, int &dx, int &dy) {
  // 9 行 × 9 格 + 8 个换行 = 89 字符，无末尾换行
  if (grid.size() != 89) return false;

  int count_2 = 0, col2 = -1, row2 = -1;
  for (int r = 0; r < 9; ++r) {
    for (int c = 0; c < 9; ++c) {
      const char ch = grid[r * 10 + c];
      if (ch != '0' && ch != '1' && ch != '2') return false;
      // '1' 只允许出现在中心（10/27 审查 L3：非中心 '1' 是非法图，拒绝）
      if (ch == '1' && (r * 10 + c) != 44) return false;
      if (ch == '2') {
        ++count_2;
        col2 = c;
        row2 = r;
      }
    }
    if (r < 8 && grid[r * 10 + 9] != '\n') return false;
  }
  // '1' 必须固定在中心（第 5 行第 5 列，即 0 开始的下标 [4][4]）
  if (grid[44] != '1') return false;
  // 协议：至多一个 '2'
  if (count_2 > 1) return false;

  if (count_2 == 0) {
    // 图中没有 '2'：实体与自身重叠
    dx = 0;
    dy = 0;
    return true;
  }
  // 位移 = [2所在列-5, 2所在行-5]（1 开始编号）=> 0 开始编号减 4
  dx = col2 - 4;
  dy = row2 - 4;
  return true;
}

bool parse_perception(const std::string &json_text, Perception &out,
                      std::vector<std::string> &warnings) {
  out = Perception{};  // 先清空，避免残留上一帧数据

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(json_text);
  } catch (const std::exception &e) {
    warnings.push_back(std::string("感知 JSON 解析失败：") + e.what());
    return false;
  }
  if (!j.is_object()) {
    warnings.push_back("感知 JSON 不是对象");
    return false;
  }

  // ---- 核心字段：动作必须原样回填，缺一不可 ----
  if (!read_int(j, "tick", out.tick) ||
      !read_string(j, "session_id", out.session_id)) {
    warnings.push_back("tick/session_id 缺失或类型不符，本帧不可用于动作回填");
    return false;
  }

  // ---- vision_radius（0–4） ----
  if (!read_int(j, "vision_radius", out.vision_radius) ||
      out.vision_radius < 0 || out.vision_radius > 4) {
    warnings.push_back("vision_radius 缺失或超出 0–4，按 0 处理");
    out.vision_radius = 0;
  }

  // ---- map（仅尺寸；提前解析：self.position 与实体绝对坐标的边界校验需要它） ----
  auto map_it = j.find("map");
  if (map_it != j.end() && !map_it->is_object()) {
    warnings.push_back("map 不是对象，按缺失处理");
    map_it = j.end();
  }
  if (map_it != j.end()) {
    read_int(*map_it, "width", out.map_width);
    read_int(*map_it, "height", out.map_height);
    if (out.map_width <= 0 || out.map_height <= 0) {
      warnings.push_back("map 尺寸非法（<=0），按 0 处理");
      out.map_width = 0;
      out.map_height = 0;
    }
  }

  // ---- self ----
  auto self_it = j.find("self");
  if (self_it == j.end() || !self_it->is_object()) {
    warnings.push_back("self 缺失或不是对象");
  } else {
    const nlohmann::json &s = *self_it;
    SelfState &self = out.self;
    read_string(s, "team", self.team);
    read_string(s, "robot_id", self.robot_id);
    if (read_string(s, "position_reason", self.position_reason)) {
      self.has_position_reason = true;
    }
    // 绝对坐标数组（协议：与实体的相对位置图是两种类型，必须分开解析）
    if (read_position(s, "position", self.x, self.y)) {
      self.has_position = true;
      // 边界校验（10/27 审查 L2）：坐标必须在 [0,W)×[0,H) 内，
      // 越界的异常帧按"无位置"处理（策略红线 → wait），不进策略计算。
      if (out.map_width > 0 && out.map_height > 0 &&
          (self.x < 0 || self.y < 0 || self.x >= out.map_width ||
           self.y >= out.map_height)) {
        warnings.push_back("self.position 超出地图范围，按无位置处理");
        self.has_position = false;
      }
    } else {
      warnings.push_back("self.position 缺失或类型不符");
    }
    // 上一 Tick 回执：开局为 null
    auto last_it = s.find("last_action");
    if (last_it != s.end() && !last_it->is_null()) {
      if (last_it->is_object()) {
        self.has_last_action = true;
        read_int(*last_it, "tick", self.last_tick);
        read_string(*last_it, "action", self.last_action);
        read_bool(*last_it, "moved", self.last_moved);
        if (read_string(*last_it, "effective_direction",
                        self.last_effective_direction)) {
          self.has_last_direction = true;
        }
        if (read_string(*last_it, "error", self.last_error)) {
          self.has_last_error = true;
        }
      } else {
        warnings.push_back("self.last_action 非 null 也非对象");
      }
    }
    if (read_int(s, "hp", self.hp)) {
      if (self.hp < 0) {
        warnings.push_back("self.hp 为负值，按缺失处理");
      } else {
        self.has_hp = true;
      }
    }
    if (read_bool(s, "alive", self.alive)) self.has_alive = true;
    read_bool(s, "defending", self.defending);
    if (read_int(s, "respawn_in", self.respawn_in) && self.respawn_in < 0) {
      warnings.push_back("self.respawn_in 为负值，按 0 处理");
      self.respawn_in = 0;
    }
    read_bool(s, "protected", self.protected_flag);
  }

  // ---- visible_entities（阵亡时为空数组） ----
  auto entities_it = j.find("visible_entities");
  if (entities_it == j.end() || !entities_it->is_array()) {
    warnings.push_back("visible_entities 缺失或不是数组");
  } else {
    for (const auto &e : *entities_it) {
      if (!e.is_object()) {
        warnings.push_back("visible_entities 中存在非对象条目，已跳过");
        continue;
      }
      Entity ent;
      if (!read_string(e, "type", ent.type)) {
        warnings.push_back("实体缺少 type，已跳过");
        continue;
      }
      // 官方协议 PDF 行文表格曾写作 resource_hi（简写）；线上 JSON 示例与实测均为
      // resource_high。此处做协议层归一化，防正式场按简写出值导致高价值资源被漏认。
      if (ent.type == "resource_hi") ent.type = "resource_high";
      read_string(e, "id", ent.id);
      read_string(e, "team", ent.team);
      read_string(e, "robot_id", ent.robot_id);
      if (read_int(e, "hp", ent.hp)) {
        if (ent.hp < 0) {
          warnings.push_back("实体 hp 为负值，按缺失处理");
        } else {
          ent.has_hp = true;
        }
      }
      read_bool(e, "defending", ent.defending);
      read_bool(e, "protected", ent.protected_flag);
      if (read_int(e, "value", ent.value)) {
        if (ent.value < 0) {
          warnings.push_back("实体 value 为负值，按缺失处理");
        } else {
          ent.has_value = true;
        }
      }

      // 9×9 相对位置图 -> 相对位移 -> 绝对坐标
      std::string grid;
      if (!read_string(e, "position", grid) ||
          !parse_relative_position(grid, ent.rel_dx, ent.rel_dy)) {
        warnings.push_back("实体 " +
                           (ent.id.empty() ? ent.type : ent.id) +
                           " 的位置图解析失败，跳过该实体");
        continue;
      }
      ent.has_relative = true;
      if (out.self.has_position) {
        // 绝对坐标 = self.position + 相对位移；落在地图外视为异常帧数据，
        // 保留相对位移、不给绝对坐标（策略 BFS 只消费地图内坐标）。
        const int ax = out.self.x + ent.rel_dx;
        const int ay = out.self.y + ent.rel_dy;
        if (out.map_width > 0 && out.map_height > 0 &&
            (ax < 0 || ay < 0 || ax >= out.map_width || ay >= out.map_height)) {
          warnings.push_back("实体绝对坐标超出地图范围，仅保留相对位移");
        } else {
          ent.abs_x = ax;
          ent.abs_y = ay;
          ent.has_absolute = true;
        }
      }
      out.entities.push_back(std::move(ent));
    }
  }

  // ---- score ----
  auto score_it = j.find("score");
  if (score_it != j.end() && !score_it->is_object()) {
    warnings.push_back("score 不是对象，按缺失处理");
    score_it = j.end();
  }
  if (score_it != j.end()) {
    const nlohmann::json &sc = *score_it;
    read_int(sc, "resource_value", out.score.resource_value);
    read_number(sc, "effective_damage", out.score.effective_damage);
    read_int(sc, "mission_completed", out.score.mission_completed);
    read_int(sc, "invalid_actions", out.score.invalid_actions);
    auto ids_it = sc.find("mission_completed_ids");
    if (ids_it != sc.end() && ids_it->is_array()) {
      for (const auto &id : *ids_it) {
        if (id.is_string()) {
          out.score.mission_completed_ids.push_back(id.get<std::string>());
        }
      }
    }
  }

  // ---- mission_text / missions ----
  read_string(j, "mission_text", out.mission_text);
  auto missions_it = j.find("missions");
  if (missions_it != j.end() && !missions_it->is_array()) {
    warnings.push_back("missions 不是数组，按缺失处理");
    missions_it = j.end();
  }
  if (missions_it != j.end()) {
    for (const auto &m : *missions_it) {
      if (!m.is_object()) {
        warnings.push_back("missions 中存在非对象条目，已跳过");
        continue;
      }
      Mission mission;
      if (!read_string(m, "id", mission.id) ||
          !read_string(m, "type", mission.type)) {
        warnings.push_back("任务缺少 id/type，已跳过");
        continue;
      }
      if (read_int(m, "target", mission.target)) {
        if (mission.target < 0) {
          warnings.push_back("任务 target 为负值，按缺失处理");
        } else {
          mission.has_target = true;
        }
      }
      if (read_position(m, "position", mission.pos_x, mission.pos_y)) {
        mission.has_position = true;
      }
      out.missions.push_back(std::move(mission));
    }
  }

  // ---- event_text ----
  read_string(j, "event_text", out.event_text);

  return true;
}

}  // namespace rmtc_bot
