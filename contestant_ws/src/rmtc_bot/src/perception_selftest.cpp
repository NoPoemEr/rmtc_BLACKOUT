// 感知解析自检程序（不依赖 ROS，可独立运行）：
//   ros2 run rmtc_bot perception_selftest
// 用例全部来自通信协议原文示例 + 各类非法输入。
// 退出码 0 = 全部通过，1 = 有失败。

#include <cstdio>
#include <string>
#include <vector>

#include "rmtc_bot/perception.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *name) {
  ++g_checks;
  if (ok) {
    std::printf("[PASS] %s\n", name);
  } else {
    std::printf("[FAIL] %s\n", name);
    ++g_failures;
  }
}

// 协议 §3 定位示例：'2' 在第 3 行第 6 列 -> 相对位移 [1,-2]（右 1 上 2）
const char *k_grid_right1_up2 =
    "000000000\n"
    "000000000\n"
    "000002000\n"
    "000000000\n"
    "000010000\n"
    "000000000\n"
    "000000000\n"
    "000000000\n"
    "000000000";

// 无 '2'：与自身重叠
const char *k_grid_center_only =
    "000000000\n"
    "000000000\n"
    "000000000\n"
    "000000000\n"
    "000010000\n"
    "000000000\n"
    "000000000\n"
    "000000000\n"
    "000000000";

// 协议感知示例中 robot_B / n09 的图：'2' 在第 6 行第 6 列 -> [1,1]
const char *k_grid_right1_down1 =
    "000000000\n"
    "000000000\n"
    "000000000\n"
    "000000000\n"
    "000010000\n"
    "000002000\n"
    "000000000\n"
    "000000000\n"
    "000000000";

// 协议感知示例中 h01 的图：'2' 在第 4 行第 7 列 -> [2,-1]
const char *k_grid_high =
    "000000000\n"
    "000000000\n"
    "000000000\n"
    "000000200\n"
    "000010000\n"
    "000000000\n"
    "000000000\n"
    "000000000\n"
    "000000000";

// 协议感知示例中 obstacle 的图：'2' 在第 6 行第 8 列 -> [3,1]
const char *k_grid_obstacle =
    "000000000\n"
    "000000000\n"
    "000000000\n"
    "000000000\n"
    "000010000\n"
    "000000020\n"
    "000000000\n"
    "000000000\n"
    "000000000";

void test_relative_position() {
  int dx = 0, dy = 0;
  bool ok;

  ok = rmtc_bot::parse_relative_position(k_grid_right1_up2, dx, dy);
  check(ok && dx == 1 && dy == -2, "相对图: 右1上2 -> [1,-2]");

  ok = rmtc_bot::parse_relative_position(k_grid_center_only, dx, dy);
  check(ok && dx == 0 && dy == 0, "相对图: 无'2' -> [0,0]");

  ok = rmtc_bot::parse_relative_position(k_grid_right1_down1, dx, dy);
  check(ok && dx == 1 && dy == 1, "相对图: 右1下1 -> [1,1]");

  ok = rmtc_bot::parse_relative_position(k_grid_high, dx, dy);
  check(ok && dx == 2 && dy == -1, "相对图: h01 -> [2,-1]");

  ok = rmtc_bot::parse_relative_position(k_grid_obstacle, dx, dy);
  check(ok && dx == 3 && dy == 1, "相对图: obstacle -> [3,1]");

  // 非法输入
  ok = rmtc_bot::parse_relative_position("too short", dx, dy);
  check(!ok, "相对图: 长度错误 -> false");

  std::string no_center = k_grid_center_only;
  no_center[44] = '0';
  ok = rmtc_bot::parse_relative_position(no_center, dx, dy);
  check(!ok, "相对图: '1' 不在中心 -> false");

  std::string two_markers = k_grid_right1_up2;
  two_markers[3] = '2';  // 第一行再放一个 '2'
  ok = rmtc_bot::parse_relative_position(two_markers, dx, dy);
  check(!ok, "相对图: 两个 '2' -> false");

  std::string bad_char = k_grid_center_only;
  bad_char[0] = 'x';
  ok = rmtc_bot::parse_relative_position(bad_char, dx, dy);
  check(!ok, "相对图: 非法字符 -> false");

  std::string bad_newline = k_grid_center_only;
  bad_newline[9] = '0';  // 换行位置放格子字符
  ok = rmtc_bot::parse_relative_position(bad_newline, dx, dy);
  check(!ok, "相对图: 换行位置错误 -> false");

  // 非中心 '1'（10/27 审查 L3）：'1' 只允许在中心
  std::string extra_one = k_grid_center_only;
  extra_one[0] = '1';
  ok = rmtc_bot::parse_relative_position(extra_one, dx, dy);
  check(!ok, "相对图: 非中心出现 '1' -> false");

  // 把换行写成字面 '\\' 和 'n' 两个字符（新手常见错误）：长度变 97 -> false
  std::string literal_backslash_n =
      "000000000\\n000000000\\n000000000\\n000000000\\n000010000\\n000000000"
      "\\n000000000\\n000000000\\n000000000";
  ok = rmtc_bot::parse_relative_position(literal_backslash_n, dx, dy);
  check(!ok, "相对图: 字面 \\n 两字符 -> false");

  // 末尾多一个换行：长度 90 -> false
  std::string trailing_newline = k_grid_center_only;
  trailing_newline += '\n';
  ok = rmtc_bot::parse_relative_position(trailing_newline, dx, dy);
  check(!ok, "相对图: 末尾换行 -> false");
}

// 通信协议 §3 感知示例（原样结构）
const char *k_perception_example = R"json({
  "tick": 148,
  "session_id": "example-match",
  "vision_radius": 4,
  "self": {"team": "A", "position": [8, 8], "hp": 70, "alive": true,
           "defending": false, "respawn_in": 0, "protected": false,
           "robot_id": "A",
           "last_action": {"tick": 147, "action": "move", "moved": true,
                           "effective_direction": "right"}},
  "visible_entities": [
    {"type": "robot", "id": "robot_B", "team": "B", "robot_id": "B",
     "position": "000000000\n000000000\n000000000\n000000000\n000010000\n000002000\n000000000\n000000000\n000000000",
     "hp": 60, "defending": false, "protected": false},
    {"type": "resource_normal", "id": "n09",
     "position": "000000000\n000000000\n000000000\n000000000\n000010000\n000002000\n000000000\n000000000\n000000000",
     "value": 10},
    {"type": "resource_high", "id": "h01",
     "position": "000000000\n000000000\n000000000\n000000200\n000010000\n000000000\n000000000\n000000000\n000000000",
     "value": 20},
    {"type": "obstacle",
     "position": "000000000\n000000000\n000000000\n000000000\n000010000\n000000020\n000000000\n000000000\n000000000"}
  ],
  "score": {"resource_value": 30, "effective_damage": 0,
            "mission_completed": 1, "mission_completed_ids": ["collect_high_value"],
            "invalid_actions": 0},
  "mission_text": "获得至少两个普通资源；获得至少一个高价值资源；比赛结束前进入坐标 [12, 12]",
  "missions": [
    {"id": "collect_two_normal", "type": "normal_resources", "target": 2},
    {"id": "collect_high_value", "type": "high_resources", "target": 1},
    {"id": "reach_target", "type": "reach", "target": 1, "position": [12, 12]}
  ],
  "map": {"width": 24, "height": 24},
  "event_text": ""
})json";

void test_perception_example() {
  rmtc_bot::Perception p;
  std::vector<std::string> warnings;
  const bool ok = rmtc_bot::parse_perception(k_perception_example, p, warnings);

  check(ok, "感知示例: 整体解析成功");
  check(p.tick == 148, "感知示例: tick=148");
  check(p.session_id == "example-match", "感知示例: session_id 原样");
  check(p.vision_radius == 4, "感知示例: vision_radius=4");
  check(p.self.has_position && p.self.x == 8 && p.self.y == 8,
        "感知示例: self.position=[8,8]");
  check(p.self.has_hp && p.self.hp == 70, "感知示例: hp=70");
  check(p.self.has_alive && p.self.alive, "感知示例: alive=true");
  check(p.self.protected_flag == false, "感知示例: protected=false");
  check(p.self.has_last_action && p.self.last_tick == 147 &&
            p.self.last_action == "move" && p.self.last_moved &&
            p.self.has_last_direction &&
            p.self.last_effective_direction == "right",
        "感知示例: last_action 回执解析");
  check(!p.self.has_last_error, "感知示例: last_action 无 error");

  check(p.entities.size() == 4, "感知示例: 实体数=4");
  bool robot_ok = false, normal_ok = false, high_ok = false, obstacle_ok = false;
  for (const auto &e : p.entities) {
    if (e.type == "robot" && e.id == "robot_B" && e.team == "B" &&
        e.has_absolute && e.abs_x == 9 && e.abs_y == 9 && e.has_hp &&
        e.hp == 60) {
      robot_ok = true;
    }
    if (e.type == "resource_normal" && e.id == "n09" && e.has_value &&
        e.value == 10 && e.has_absolute && e.abs_x == 9 && e.abs_y == 9) {
      normal_ok = true;
    }
    if (e.type == "resource_high" && e.id == "h01" && e.has_value &&
        e.value == 20 && e.has_absolute && e.abs_x == 10 && e.abs_y == 7) {
      high_ok = true;
    }
    if (e.type == "obstacle" && e.id.empty() && e.has_absolute &&
        e.abs_x == 11 && e.abs_y == 9) {
      obstacle_ok = true;
    }
  }
  check(robot_ok, "感知示例: robot_B 绝对坐标 [9,9]");
  check(normal_ok, "感知示例: n09 绝对坐标 [9,9] value=10");
  check(high_ok, "感知示例: h01 绝对坐标 [10,7] value=20");
  check(obstacle_ok, "感知示例: obstacle 绝对坐标 [11,9] 无 id");

  check(p.score.resource_value == 30 && p.score.effective_damage == 0 &&
            p.score.mission_completed == 1 &&
            p.score.invalid_actions == 0 &&
            p.score.mission_completed_ids.size() == 1 &&
            p.score.mission_completed_ids[0] == "collect_high_value",
        "感知示例: score 解析");

  check(p.missions.size() == 3, "感知示例: 任务数=3");
  bool reach_ok = false;
  for (const auto &m : p.missions) {
    if (m.id == "reach_target" && m.type == "reach" && m.has_position &&
        m.pos_x == 12 && m.pos_y == 12 && m.has_target && m.target == 1) {
      reach_ok = true;
    }
  }
  check(reach_ok, "感知示例: reach 任务 [12,12]");

  check(p.map_width == 24 && p.map_height == 24, "感知示例: 地图 24x24");
  check(p.event_text.empty(), "感知示例: event_text 为空");
  check(warnings.empty(), "感知示例: 无字段警告");
}

void test_malformed_perceptions() {
  rmtc_bot::Perception p;
  std::vector<std::string> warnings;

  // 非 JSON
  check(!rmtc_bot::parse_perception("not json", p, warnings),
        "非法感知: 非 JSON -> false");

  // 缺 tick
  check(!rmtc_bot::parse_perception(R"({"session_id":"s1"})", p, warnings),
        "非法感知: 缺 tick -> false");

  // tick 是字符串
  check(!rmtc_bot::parse_perception(
            R"({"tick":"148","session_id":"s1"})", p, warnings),
        "非法感知: tick 为字符串 -> false");

  // tick 是浮点 148.0（协议要求整数）
  check(!rmtc_bot::parse_perception(
            R"({"tick":148.0,"session_id":"s1"})", p, warnings),
        "非法感知: tick 为浮点 -> false");

  // 缺 session_id
  check(!rmtc_bot::parse_perception(R"({"tick":1})", p, warnings),
        "非法感知: 缺 session_id -> false");

  // 核心字段合法，实体位置图是字面 \n：整帧可用，实体被跳过并警告
  const std::string bad_grid = R"json({
    "tick": 2, "session_id": "s2",
    "self": {"position": [5, 5]},
    "visible_entities": [
      {"type": "resource_normal", "id": "bad",
       "position": "000000000\\n000000000\\n000000000\\n000000000\\n000010000\\n000000000\\n000000000\\n000000000\\n000000000"}
    ]
  })json";
  warnings.clear();
  check(rmtc_bot::parse_perception(bad_grid, p, warnings),
        "非法感知: 核心字段合法时整帧仍可用");
  check(p.entities.empty(), "非法感知: 坏位置图实体被跳过");
  check(!warnings.empty(), "非法感知: 产生警告");

  // self.position 缺失：整帧可用，实体保留但无绝对坐标
  const std::string no_self_pos = R"json({
    "tick": 3, "session_id": "s3",
    "self": {},
    "visible_entities": [
      {"type": "obstacle",
       "position": "000000000\n000000000\n000000000\n000000000\n000010000\n000000020\n000000000\n000000000\n000000000"}
    ]
  })json";
  warnings.clear();
  check(rmtc_bot::parse_perception(no_self_pos, p, warnings),
        "非法感知: 缺 self.position 仍可用");
  check(p.entities.size() == 1 && p.entities[0].has_relative &&
            !p.entities[0].has_absolute,
        "非法感知: 实体有相对位移无绝对坐标");
}

// 练习包 stage2 实际配置风格：reach 任务只有 position 没有 target；地图 18x18
// （正式协议示例是 24x24 且 reach 带 target——两种都得能解析）
void test_real_scenarios() {
  rmtc_bot::Perception p;
  std::vector<std::string> warnings;

  // 场景 1：练习 stage2 配置风格（reach 无 target、18x18、带任务文本）
  const std::string practice_style = R"json({
    "tick": 60, "session_id": "practice-8b94bdd1a72943b0bfc8f90389f1eb70",
    "vision_radius": 4,
    "self": {"team": "A", "position": [1, 1], "hp": 100, "alive": true,
             "defending": false, "respawn_in": 0, "protected": false,
             "robot_id": "A"},
    "visible_entities": [],
    "score": {"resource_value": 0, "effective_damage": 0,
              "mission_completed": 0, "invalid_actions": 0},
    "mission_text": "练习任务：收集两个普通资源、一个高价值资源，并到达 [9,9]。",
    "missions": [
      {"id": "collect_two_normal", "type": "normal_resources", "target": 2},
      {"id": "collect_high_value", "type": "high_resources", "target": 1},
      {"id": "reach_target", "type": "reach", "position": [9, 9]}
    ],
    "map": {"width": 18, "height": 18},
    "event_text": ""
  })json";
  warnings.clear();
  check(rmtc_bot::parse_perception(practice_style, p, warnings),
        "真实场景: 练习 stage2 风格解析成功");
  check(p.missions.size() == 3, "真实场景: 任务数=3");
  bool reach_ok = false;
  for (const auto &m : p.missions) {
    if (m.id == "reach_target" && m.type == "reach" && m.has_position &&
        m.pos_x == 9 && m.pos_y == 9 && !m.has_target) {
      reach_ok = true;
    }
  }
  check(reach_ok, "真实场景: reach 有 position 无 target 可解析");
  check(p.map_width == 18 && p.map_height == 18, "真实场景: 地图 18x18");
  check(warnings.empty(), "真实场景: 无字段警告");

  // 场景 2：阵亡帧——visible_entities 为空、last_action 带 E_STATE
  const std::string dead_frame = R"json({
    "tick": 61, "session_id": "practice-8b94bdd1a72943b0bfc8f90389f1eb70",
    "vision_radius": 4,
    "self": {"team": "A", "position": [5, 5], "hp": 0, "alive": false,
             "defending": false, "respawn_in": 2, "protected": false,
             "robot_id": "A",
             "last_action": {"tick": 60, "action": "move", "moved": false,
                             "error": "E_STATE"}},
    "visible_entities": [],
    "score": {"resource_value": 0, "effective_damage": 0,
              "mission_completed": 0, "invalid_actions": 0},
    "missions": [],
    "map": {"width": 18, "height": 18},
    "event_text": ""
  })json";
  warnings.clear();
  check(rmtc_bot::parse_perception(dead_frame, p, warnings),
        "真实场景: 阵亡帧解析成功");
  check(p.self.has_alive && !p.self.alive, "真实场景: alive=false");
  check(p.self.respawn_in == 2, "真实场景: respawn_in=2");
  check(p.self.has_last_error && p.self.last_error == "E_STATE",
        "真实场景: last_action 带 E_STATE 回执");
  check(p.entities.empty(), "真实场景: 阵亡时实体为空");
  check(warnings.empty(), "真实场景: 阵亡帧无警告");
}

// 回归（10/21 stage2 线上实测发现）：score.effective_damage 在线上是浮点
// （规则允许小数，如残血多车分摊按比例计贡献；练习网关原样发 20.0/260.0），
// 旧 read_int 只收整数 → 恒解析失败得 0，导致线上"打了半天伤害=0"。
// 本用例锁死：浮点必须收下；整数保持兼容；非数不崩溃。
void test_float_effective_damage() {
  rmtc_bot::Perception p;
  std::vector<std::string> warnings;

  const std::string float_damage = R"json({
    "tick": 58, "session_id": "practice-xxx",
    "self": {"team": "A", "position": [1, 1], "hp": 80, "alive": true,
             "defending": false, "respawn_in": 0, "protected": false,
             "robot_id": "A"},
    "visible_entities": [],
    "score": {"resource_value": 60, "effective_damage": 123.5,
              "mission_completed": 3, "invalid_actions": 0},
    "missions": [],
    "map": {"width": 18, "height": 18},
    "event_text": ""
  })json";
  warnings.clear();
  check(rmtc_bot::parse_perception(float_damage, p, warnings),
        "浮点伤害: 解析成功");
  check(p.score.effective_damage == 123.5,
        "浮点伤害: effective_damage=123.5（线上真实值形态）");

  const std::string int_damage = R"json({
    "tick": 59, "session_id": "practice-xxx",
    "self": {"team": "A", "position": [1, 1], "hp": 80, "alive": true,
             "defending": false, "respawn_in": 0, "protected": false,
             "robot_id": "A"},
    "visible_entities": [],
    "score": {"resource_value": 60, "effective_damage": 20,
              "mission_completed": 3, "invalid_actions": 0},
    "missions": [],
    "map": {"width": 18, "height": 18},
    "event_text": ""
  })json";
  warnings.clear();
  check(rmtc_bot::parse_perception(int_damage, p, warnings),
        "整数伤害: 解析成功（兼容旧形态）");
  check(p.score.effective_damage == 20.0, "整数伤害: effective_damage=20");

  const std::string bad_damage = R"json({
    "tick": 60, "session_id": "practice-xxx",
    "self": {"team": "A", "position": [1, 1], "hp": 80, "alive": true,
             "defending": false, "respawn_in": 0, "protected": false,
             "robot_id": "A"},
    "visible_entities": [],
    "score": {"resource_value": 60, "effective_damage": "oops",
              "mission_completed": 3, "invalid_actions": 0},
    "missions": [],
    "map": {"width": 18, "height": 18},
    "event_text": ""
  })json";
  warnings.clear();
  check(rmtc_bot::parse_perception(bad_damage, p, warnings) &&
            p.score.effective_damage == 0.0,
        "非数伤害: 不崩溃、保持 0");
}

// 协议 PDF 行文表格把高价值资源简写为 resource_hi（JSON 示例与实测为 resource_high）。
// 协议层归一化：resource_hi -> resource_high，防正式场按简写出值导致漏认。
void test_resource_hi_alias() {
  rmtc_bot::Perception p;
  std::vector<std::string> warnings;
  const std::string hi_alias = R"json({
    "tick": 60, "session_id": "practice-xxx",
    "self": {"team": "A", "position": [1, 1], "hp": 80, "alive": true,
             "defending": false, "respawn_in": 0, "protected": false,
             "robot_id": "A"},
    "visible_entities": [
      {"type": "resource_hi", "id": "h01", "position":
        "000000000\n000000000\n000000000\n000000000\n000010000\n000000000\n000000000\n000000000\n000000000",
       "value": 20}
    ],
    "score": {"resource_value": 0, "effective_damage": 0,
              "mission_completed": 0, "invalid_actions": 0},
    "missions": [],
    "map": {"width": 18, "height": 18},
    "event_text": ""
  })json";
  warnings.clear();
  check(rmtc_bot::parse_perception(hi_alias, p, warnings),
        "resource_hi 别名: 解析成功");
  check(p.entities.size() == 1 && p.entities[0].type == "resource_high" &&
            p.entities[0].has_value && p.entities[0].value == 20,
        "resource_hi 别名: 归一化为 resource_high、value 保留");
}

// 10/27 审查 H1/L1/L2 回归：超大整数不得静默按 32 位回绕；
// 越界坐标/负值字段按缺失降级并警告，绝不进策略计算。
void test_bounded_integers_and_bounds() {
  rmtc_bot::Perception p;
  std::vector<std::string> warnings;

  // H1a：tick=2^32（回绕成 0 会毒化 last_tick_）→ 整帧拒绝回填
  warnings.clear();
  check(!rmtc_bot::parse_perception(
            R"({"tick":4294967296,"session_id":"s1"})", p, warnings),
        "有界整数: tick=2^32 -> false（不回填）");
  check(!warnings.empty(), "有界整数: tick 越界产生警告");

  // H1b：vision_radius=2^32 回绕成 0 会静默通过 0–4 校验 → 按越界处理并警告
  warnings.clear();
  check(rmtc_bot::parse_perception(
            R"({"tick":1,"session_id":"s1","vision_radius":4294967296})",
            p, warnings),
        "有界整数: vision_radius=2^32 -> 整帧仍可用");
  check(p.vision_radius == 0, "有界整数: vision_radius 按 0 处理");
  check(!warnings.empty(), "有界整数: vision_radius 越界产生警告");

  // L2a：self.position 越界坐标（2^32 回绕 + 地图外）→ 按无位置降级
  warnings.clear();
  check(rmtc_bot::parse_perception(
            R"json({"tick":2,"session_id":"s2",
              "self":{"position":[4294967296,2]},
              "map":{"width":18,"height":18}})json",
            p, warnings),
        "越界坐标: 超大 self.position -> 整帧仍可用");
  check(!p.self.has_position, "越界坐标: self.position 按无位置降级");

  // L2b：合法整数但落在地图外 → 同样按无位置降级
  warnings.clear();
  check(rmtc_bot::parse_perception(
            R"json({"tick":3,"session_id":"s3",
              "self":{"position":[30,5]},
              "map":{"width":18,"height":18}})json",
            p, warnings),
        "越界坐标: 地图外 self.position -> 整帧仍可用");
  check(!p.self.has_position, "越界坐标: 地图外 position 按无位置降级");

  // L2c：实体绝对坐标越界（self 在角落，相对图向右下）→ 保留相对、不给绝对
  warnings.clear();
  const std::string edge_entity = R"json({
    "tick": 4, "session_id": "s4",
    "self": {"position": [17, 17]},
    "visible_entities": [
      {"type": "resource_normal", "id": "edge",
       "position": "000000000\n000000000\n000000000\n000000000\n000010000\n000002000\n000000000\n000000000\n000000000"}
    ],
    "map": {"width": 18, "height": 18}
  })json";
  check(rmtc_bot::parse_perception(edge_entity, p, warnings),
        "越界实体: 解析成功");
  check(p.entities.size() == 1 && p.entities[0].has_relative &&
            !p.entities[0].has_absolute,
        "越界实体: 绝对坐标越界 -> 仅保留相对位移");

  // L2d：负值字段降级
  warnings.clear();
  check(rmtc_bot::parse_perception(
            R"json({"tick":5,"session_id":"s5",
              "self":{"position":[5,5],"hp":-5,"respawn_in":-1},
              "visible_entities":[
                {"type":"resource_normal","id":"n1","value":-3,
                 "position":"000000000\n000000000\n000000000\n000000000\n000010000\n000002000\n000000000\n000000000\n000000000"}],
              "missions":[{"id":"m1","type":"reach","target":-2,"position":[1,1]}],
              "map":{"width":18,"height":18}})json",
            p, warnings),
        "负值字段: 解析成功");
  check(!p.self.has_hp, "负值字段: hp<0 按缺失处理");
  check(p.self.respawn_in == 0, "负值字段: respawn_in<0 按 0 处理");
  check(p.entities.size() == 1 && !p.entities[0].has_value,
        "负值字段: value<0 按缺失处理");
  check(p.missions.size() == 1 && !p.missions[0].has_target,
        "负值字段: target<0 按缺失处理");

  // L2e：map 尺寸非法 → 按 0 处理并警告（策略红线会 wait）
  warnings.clear();
  check(rmtc_bot::parse_perception(
            R"json({"tick":6,"session_id":"s6","map":{"width":-1,"height":0}})json",
            p, warnings),
        "非法地图: 解析成功");
  check(p.map_width == 0 && p.map_height == 0, "非法地图: 尺寸按 0 处理");
  check(!warnings.empty(), "非法地图: 产生警告");

  // L4：score/missions 类型错 → 警告但不崩
  warnings.clear();
  check(rmtc_bot::parse_perception(
            R"json({"tick":7,"session_id":"s7","score":[1,2],"missions":{"a":1}})json",
            p, warnings),
        "类型错: score/missions 非对象/非数组 -> 解析成功");
  check(!warnings.empty(), "类型错: 产生警告");

  // 开局 last_action 为 null → has_last_action=false、无警告
  warnings.clear();
  check(rmtc_bot::parse_perception(
            R"json({"tick":8,"session_id":"s8",
              "self":{"position":[5,5],"last_action":null}})json",
            p, warnings),
        "开局回执: last_action=null -> 解析成功");
  check(!p.self.has_last_action, "开局回执: has_last_action=false");

  // session_id 类型错（整数）→ 整帧拒绝
  warnings.clear();
  check(!rmtc_bot::parse_perception(R"({"tick":9,"session_id":42})", p,
                                    warnings),
        "类型错: session_id 为整数 -> false");
}

}  // namespace

int main() {
  test_relative_position();
  test_perception_example();
  test_malformed_perceptions();
  test_real_scenarios();
  test_float_effective_damage();
  test_resource_hi_alias();
  test_bounded_integers_and_bounds();

  if (g_failures == 0) {
    std::printf("\n全部通过（%d 项）。\n", g_checks);
    return 0;
  }
  std::printf("\n%d 项失败（共 %d 项）。\n", g_failures, g_checks);
  return 1;
}
