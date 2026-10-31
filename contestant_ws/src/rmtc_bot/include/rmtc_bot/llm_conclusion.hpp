#pragma once
// LLM 结论解析与本地校验（M3 管道预备）——纯"管道"模块，不含任何策略。
//
// 依据 docs/llm_prompt_design.md：
//   §1 输出 Schema（固定 JSON，与练习 mock 同构）：
//     {"goals": [...], "event_plan": {rect|points, avoid, until_tick, reason} | null,
//      "summary": "..."}
//   §4 本地校验与消费：
//     1. JSON 解析失败 → 丢弃，用上一次有效结论
//     2. goals 逐个对照 missions[].type 白名单，未知值忽略
//     3. event_plan 坐标裁剪到 [0,W)×[0,H)，非法丢弃该 plan
//     4. until_tick <= 当前 tick → 过期丢弃
//     5. 结论只是 CombatStrategy 的输入之一，动作仍由策略按协议规则生成
//
// 规则红线：event_text 必须经 LLM 理解，本地禁关键词/正则解析事件文本——本模块
// 只解析 LLM 的结构化输出，不触碰 event_text 内容本身。
//
// 解析原则：宽松读、严格用。未知键忽略（前向兼容）；坏字段逐项降级并记警告，
// 绝不因单个字段问题让整帧结论崩溃。

#include <array>
#include <string>
#include <vector>

namespace rmtc_bot {

// 事件应对建议（event_plan）
struct EventPlan {
  bool avoid = true;     // 建议避开该区域（false = 建议前往/利用）
  int until_tick = -1;   // 结论有效期；<=0 视为未设置 → 无效
  std::string reason;    // 给日志/技术报告用的说明文字

  // 区域表达二选一（解析时 rect 优先，两者都缺 → plan 无效）：
  //   rect:  [[x0,y0],[x1,y1]] 两点含端点闭区间，解析后归一化为 min/max
  //   points: [[x,y], ...] 多点集合
  bool has_rect = false;
  int rect_x0 = 0, rect_y0 = 0, rect_x1 = 0, rect_y1 = 0;  // 含端点
  std::vector<std::array<int, 2>> points;
};

struct LlmConclusion {
  std::vector<std::string> goals;  // 任务类型优先级排序（值已过类型白名单）
  bool has_plan = false;           // 本次结论是否带有有效 event_plan
  EventPlan plan;
  std::string summary;
};

// 解析 LLM 返回的结论 JSON。返回 false 表示顶层不是合法 JSON（调用方应丢弃结论、
// 沿用上一次有效结论）；true 但字段有问题时，问题记入 warnings 并逐项降级。
bool parse_conclusion(const std::string& json, LlmConclusion& out,
                      std::vector<std::string>& warnings);

// 本地校验（§4 规则的实现）：原地过滤 goals 白名单、裁剪 plan 坐标到地图内、
// 作废过期/无区域表达的 plan。返回校验后是否仍有有效 plan（调用方可直接读
// out.has_plan，返回值仅为书写方便）。
bool validate_conclusion(LlmConclusion& out, int map_w, int map_h,
                         int current_tick,
                         const std::vector<std::string>& allowed_goals,
                         std::vector<std::string>& warnings);

}  // namespace rmtc_bot
