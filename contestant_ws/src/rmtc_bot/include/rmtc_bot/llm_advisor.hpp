#pragma once
// LLM 顾问层（纯函数）：事件公告理解与态势综述的提示词构造、LLM 输出解析。
//
// 合规红线（rules §8.1）：随机事件文本理解必须由官方 AI 服务完成，本地禁止
// 正则/关键词/固定句式模板/预设日程解析。因此：
//   - 事件解析提示词只携带【公告原文 + 地图尺寸 + 当前 tick + 输出 Schema】，
//     本地不做任何文本内容提取；
//   - 本地只解析 LLM 的结构化 JSON 输出（宽松读、严格用，坏字段逐项降级）。
//
// 设计依据：docs/决策设计.md §4、§5。

#include <string>
#include <vector>

#include "rmtc_bot/decision_model.hpp"

namespace rmtc_bot {

// 事件理解提示词：把公告原文交给 LLM，要求输出固定 Schema。
// 输出 Schema（LLM 侧）：
//   {"type": "explosion|illusion|heal|fortress|world_split|chili|thorns|vulnerable|other",
//    "region": {"rect": [x0,y0,x1,y1]} 或 {"points": [[x,y],...]} 或 null,
//    "numbers": {"damage": 数值, "heal": 数值, "duration": 整数} 或 null,
//    "timing": {"active_tick": 整数, "until_tick": 整数} 或 null,
//    "reason": "一句话说明"}
std::string build_event_parse_prompt(const std::string& event_text, int map_w,
                                     int map_h, int current_tick,
                                     const DecisionParams& params);

// 态势综述提示词：信念摘要（我方分数/对手估计/事件表）+ 本地候选方案与 EV
// + 问题（排序候选并给出四类权重调整建议）。LLM 输出由 parse_weight_hints 解析。
std::string build_strategy_prompt(const Belief& b,
                                  const std::vector<GoalCandidate>& goals,
                                  const DecisionParams& params);

// 解析 LLM 的事件理解输出为 EventState（数值可随场次变化，此处只做结构与域
// 解析，不裁剪到地图——地图裁剪归 decision_model::validate_event_state）。
// 返回 false：顶层不是合法 JSON 对象（调用方丢弃）；true 但字段有问题时
// 逐项降级并记 warnings。
bool parse_event_understanding(const std::string& json, EventState& out,
                               std::vector<std::string>& warnings);

// 解析 LLM 的权重调整建议 JSON：
//   {"weights": {"resource": 1.2, "task": 0.8, "combat": 1.5, "survival": 1.0}}
// 缺键 = 不调整；类型错/非有限数 → 该字段拒收并警告。顶层非对象返回 false。
bool parse_weight_hints(const std::string& json, LlmWeightHints& out,
                        std::vector<std::string>& warnings);

// FNV-1a 64 位 → 16 位十六进制文本 hash（事件公告判重/缓存键用）。
std::string hash_text(const std::string& s);

}  // namespace rmtc_bot
