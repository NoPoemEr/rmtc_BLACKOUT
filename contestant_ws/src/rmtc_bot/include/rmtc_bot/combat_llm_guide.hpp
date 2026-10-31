#pragma once
// combat_llm_guide.hpp —— M3「LLM 意图 → 本地指令」翻译层
// （docs/combat_design.md §2 决策分层第 3 层的纯逻辑部分，10/21 提前落地）
//
// 职责（全部纯函数、无状态、离线可测；CombatStrategy 10/27 直接调用本层）：
//   1. prepare_conclusion：把 LlmBrain 的最新结果解析/校验成可直接消费的
//      LlmConclusion（对接 llm_conclusion.hpp 的宽松解析 + §4 本地校验）
//   2. missions_by_goal_priority：LLM 目标优先级 → 任务执行顺序（指针按序）
//   3. plan_active / plan_blocks_tile / redirect_avoid_plan：
//      event_plan（avoid 矩形/点位）→ 对规则层 move 候选做 1 Tick 尺度的局部修正
//
// 设计红线（与 llm_conclusion.hpp 一致）：
//   - 本层绝不解析 event_text 原文；事件理解只发生在 LLM 侧，这里只消费 LLM 的
//     结构化输出（矩形/点位/时效），不做关键词/正则匹配（规则合规审计 §五）。
//   - 本层不产生"新决策"，只做三件事：给任务排序、拦下会踩进回避区的步子、
//     给一个不更差的替代方向。规则层动作永远是主体，LLM 结论永远是修正。
//   - 结论过期/无效 → 所有函数幂等回退（返回候选原样 / 空顺序），规则层照跑。
//
// 尚未覆盖的部分（10/27 与规则层一起做，需记忆地图内核）：
//   - 跨多格绕行路径规划（本层只负责"这一格别踩进去"）
//   - goto 点位（avoid=false）的多目标排程（内核 BFS 直取即可）

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rmtc_bot/llm_client.hpp"
#include "rmtc_bot/llm_conclusion.hpp"
#include "rmtc_bot/perception.hpp"
#include "rmtc_bot/strategy.hpp"

namespace rmtc_bot {

// missions[].type 去重列表（combat_llm_guide 内部按类型对照用；注意：策略层
// validate_conclusion 的 allowed_goals 白名单是 type ∪ id，见 combat_strategy.cpp）
std::vector<std::string> mission_type_list(const std::vector<Mission> &missions);

// 单入口：LlmResult → 已解析 + 已校验的结论。
// 返回 false：不可用（状态非成功 / 顶层非 JSON / 校验后既无 goals 也无 plan），
// 调用方直接回退规则层，不要读 out。
bool prepare_conclusion(const LlmResult &result, const Perception &p,
                        LlmConclusion &out, std::vector<std::string> &warnings);

// 结论里是否有任何可用的意图（goals 非空或 event_plan 有效）
inline bool conclusion_usable(const LlmConclusion &c) {
  return !c.goals.empty() || c.has_plan;
}

// LLM 目标优先级 → 任务执行顺序（返回 missions 的指针，goals 序优先；
// missions 里没被 goals 覆盖的按原顺序追加兜底，规则层不丢目标）。
// 跳过已完成 id（completed_ids）；reach 任务无坐标的跳过（内核无法导航）。
std::vector<const Mission *> missions_by_goal_priority(
    const std::vector<std::string> &goals, const std::vector<Mission> &missions,
    const std::vector<std::string> &completed_ids);

// plan 是否仍生效（有区域表达且 until_tick 尚未到期）
bool plan_active(const LlmConclusion &c, int tick);

// 该格是否被 plan 的回避区覆盖（含端点闭区间 / 点位集合）。
// avoid=false 的 plan 是"前往利用"建议，不拦路。
bool plan_blocks_tile(const EventPlan &plan, int x, int y);

// 对规则层 move 候选做回避修正：
//   - 候选非 move / plan 非 avoid / 无区域表达 / 目的格不在回避区 → 原样返回
//   - 目的格在回避区：
//       我在区内 → 四个方向里选"最快离开回避区"的可走格（步数最小，同分取
//                    up/right/down/left 靠前者）
//       我在区外 → 只接受仍在区外的方向（宁可不走，不主动踩进危险区）
//   - 全部方向不可走（障碍/出界/都会进入回避区）→ wait
// blocked_tiles：调用方（记忆地图内核）传入的已知不可走格（障碍/敌车等）。
ActionChoice redirect_avoid_plan(
    const EventPlan &plan, int cur_x, int cur_y, int map_w, int map_h,
    const std::set<std::pair<int, int>> &blocked_tiles,
    const ActionChoice &candidate);

}  // namespace rmtc_bot
