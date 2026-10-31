#pragma once
// 感知解析模块：把 /team_X/perception 的 JSON 字符串解析成强类型结构。
//
// 这是纯协议层代码：只做"字段类型检查 + 取值 + 9×9 相对位置图换算"，
// 不含任何策略（不决定往哪走、打谁）。策略模块直接消费本模块产出的 Perception。
//
// 协议要求（通信协议 §3）：
//   - 解析失败/字段缺失不得导致进程退出；无法可靠决策时不提交臆测动作
//   - self.position 是绝对坐标数组 [x,y]；visible_entities[].position 是
//     9×9 相对位置字符串，两种类型不能混用同一种解析
//   - 9×9 图共 89 字符（81 格 + 8 换行，无末尾换行），'1' 固定中心（第 5 行
//     第 5 列），'2' 为实体所在格，'0' 为占位；不得把换行误读为 '\' 和 'n'
//   - 位移 = [2所在列-5, 2所在行-5]（1 开始编号）；图中无 '2' 表示与自身
//     重叠 [0,0]；绝对坐标 = self.position + 相对位移
//   - x 向右、y 向下；'0' 不代表该格可通行或在地图内

#include <string>
#include <vector>

namespace rmtc_bot {

// 本车状态（self.*）
struct SelfState {
  std::string team;      // 席位 A/B/C/D
  std::string robot_id;  // 车辆标识
  bool has_position = false;
  int x = 0, y = 0;      // 绝对坐标，地图左上角为 [0,0]
  bool has_position_reason = false;
  std::string position_reason;   // initial / respawn，仅开局或重生首帧出现
  bool has_last_action = false;  // 上一 Tick 回执；开局为 null
  int last_tick = 0;
  std::string last_action;            // move/attack/defend/wait
  bool last_moved = false;            // moved：成功移动为 true
  bool has_last_direction = false;
  std::string last_effective_direction;
  bool has_last_error = false;
  std::string last_error;             // E_OOB/E_BLOCKED/E_STATE 等执行错误码
  bool has_hp = false;
  int hp = 0;
  bool has_alive = false;
  bool alive = false;
  bool defending = false;       // 最近防御标记，不保证下一 Tick 继续
  int respawn_in = 0;           // 距复活 Tick 数，存活时为 0
  bool protected_flag = false;  // 出生保护期内（不能攻击）
};

// 视野内实体（visible_entities[]）
struct Entity {
  std::string type;      // robot / resource_normal / resource_high / obstacle
  std::string id;        // 资源/机器人有；obstacle 示例中无
  std::string team;      // 仅 robot
  std::string robot_id;  // 仅 robot
  bool has_relative = false;  // 9×9 图解析成功
  int rel_dx = 0, rel_dy = 0;
  bool has_absolute = false;  // self.position + 相对位移（本车坐标已知时）
  int abs_x = 0, abs_y = 0;
  bool has_hp = false;
  int hp = 0;
  bool defending = false;        // 仅 robot
  bool protected_flag = false;   // 仅 robot
  bool has_value = false;
  int value = 0;                 // 仅资源；>=20 记为 resource_high
};

// 结构化任务（missions[]），类型取值见通信协议 §3.1
struct Mission {
  std::string id;
  std::string type;  // normal_resources / high_resources / resource_value /
                     // effective_damage / reach
  bool has_target = false;
  int target = 0;             // 数量/伤害/价值目标；reach 也有 target（完成次数）
  bool has_position = false;  // 仅 reach：指定坐标
  int pos_x = 0, pos_y = 0;
};

// 本队累计得分（score.*）
struct Score {
  int resource_value = 0;
  // 线上是浮点（规则允许小数：残血多车分摊时按比例计贡献）；
  // 且感知里的该字段比记分板滞后 1 Tick（tick N 的感知显示截至 N-1 结算的伤害，
  // 10/21 stage2 实测）。策略里读它只用于监控，攻击效果以实体 hp 变化为准。
  double effective_damage = 0.0;
  int mission_completed = 0;
  int invalid_actions = 0;
  std::vector<std::string> mission_completed_ids;
};

// 一帧感知的全部公开字段
struct Perception {
  int tick = -1;
  std::string session_id;
  int vision_radius = 0;  // 0–4
  SelfState self;
  std::vector<Entity> entities;
  Score score;
  std::string mission_text;
  std::vector<Mission> missions;
  int map_width = 0, map_height = 0;
  std::string event_text;  // 近期公告，无公告为空字符串
};

// 解析一帧感知。
// 返回 false：tick 或 session_id 缺失/类型错/超出整数范围，不能可靠回填动作
//   （调用方不得发动作）。
// 返回 true：核心字段可用；次级字段问题写入 warnings，本帧可正常回填。
// 任何字段错误都不抛异常（协议硬性要求）。
// warnings 契约：只追加、不清空；调用方如需"每帧干净"，传入前自行 clear()。
// 数值护栏（10/27 起）：超出 int 范围的整数一律按缺失降级（防 32 位回绕）；
//   self.position / 实体绝对坐标须落在地图内，越界按无位置/无绝对坐标降级；
//   hp/value/target/respawn_in 负值按缺失/0 降级并警告。
bool parse_perception(const std::string &json_text, Perception &out,
                      std::vector<std::string> &warnings);

// 解析 9×9 相对位置字符串（须为 89 字符）。
// 成功时输出相对位移 dx/dy（x 向右、y 向下，单位格）；图中无 '2' 输出 [0,0]。
// 长度不符、含非法字符、换行位置错误、'1' 不在中心、多于一个 '2' 时返回 false。
bool parse_relative_position(const std::string &grid, int &dx, int &dy);

}  // namespace rmtc_bot
