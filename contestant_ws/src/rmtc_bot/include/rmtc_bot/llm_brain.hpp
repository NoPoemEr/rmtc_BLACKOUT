#pragma once
// LlmBrain —— M3 管道的最后一块：何时问、问什么、结论怎么交到策略手里。
// 纯"管道"模块，不含任何策略判断（不解析 event_text 语义、不决定任何动作）。
//
// 职责（docs/llm_prompt_design.md §2 输入构造 / §3 触发时机 的实现）：
//   1. prompt 构造：system（内置默认文案，占位符 {W}/{H} 代入地图尺寸）+
//      user（任务原文+结构 / 本车状态 / 事件公告原文——event_text 整段原样
//      嵌入，一字不改、不裁剪、不过滤）
//   2. 触发判定：新场次首帧 / 事件文本变化 / 任务结构变化 / 每 N Tick 复盘；
//      判定本身是纯函数 evaluate_trigger()，可完全离线自检
//   3. 节流与预算：两次提交最小间隔（min_gap）、失败退避（backoff）、
//      全场调用上限 + 事件预留（max_calls / reserve_for_events）
//   4. 结论持有：latest_result() 把最近一次完成的 LlmResult 交给 Strategy
//
// 规则红线（docs/规则合规审计.md §5）：
//   - event_text 只做"整串相等比较"判断公告是否变化——这是消息级去重，
//     不是语义解析；本模块没有任何对 event_text 的关键词/正则/模板分支，
//     内容原样交给 LLM 理解
//   - 本模块不决定任何动作；动作永远由 Strategy 按协议规则生成
//
// 内置参数与提示词是 10/21 的默认稿（对应设计稿 §6 讨论点），M3（10/27）
// 敲定讨论点后改常量并重建即可；现场只允许改 decision_config.json 的
// decision_type，本模块现场零改动。
//
// 线程约束（10/27 审查 L17 补声明）：本类内部状态无锁，仅限单线程使用
// （ROS 单线程 executor 回调串行）；bot_node 即单线程 spin。若将来换
// 多线程 executor，需自行加锁。

#include <memory>
#include <string>

#include "rmtc_bot/llm_client.hpp"
#include "rmtc_bot/perception.hpp"

namespace rmtc_bot {

// 触发/节流/预算参数（内置默认，M3 定稿点）
struct LlmBrainParams {
  int review_every_ticks = 25;         // 周期复盘间隔（§3 兜底节奏）
  int min_gap_ticks = 5;               // 两次提交最小间隔（事件预告提前 5 Tick，
                                       // 预告期内必有一次窗口）
  int max_calls_per_match = 30;        // 全场调用上限（官方配额 100，目标 10~30）
  int reserve_for_events = 3;          // 接近上限时只保"事件变化"类触发
  int backoff_ticks_after_failure = 10;  // 调用失败（超时/网络/网关错）后退避
};

// 一次触发判定的原因（无触发 = NONE）
enum class TriggerReason {
  NONE,
  SESSION_START,   // 新场次首帧：保证尽早有一次调用（AI 资格 ≥1 成功/场）
  EVENT_CHANGE,    // event_text 内容变化（最高优先业务触发）
  MISSION_CHANGE,  // 任务结构变化（missions 或 mission_completed_ids 变化）
  PERIODIC,        // 每 N Tick 复盘兜底
};

const char *trigger_reason_name(TriggerReason r);

// 触发引擎的纯状态（每场次清零；自检直接构造）
struct BrainState {
  bool seen_any = false;        // 本场是否已观察过帧（首帧触发 SESSION_START）
  int last_submit_tick = -1;    // 上次成功入队的 tick
  int backoff_until_tick = 0;   // 失败退避截止 tick（< 该 tick 不触发）
  int submitted = 0;            // 本场已成功入队次数
  std::string last_event_text;  // 上次提交时的公告原文（变化检测基准）
  std::string last_mission_sig; // 上次提交时的任务结构签名
};

// 纯函数：本帧是否值得触发一次调用（无副作用，离线可测）。
// client_enabled 由调用方从 LlmClient::enabled() 读取后传入
// （enabled 语义已包含配额/鉴权永久降级，L13 清理后不再单独传降级标志）。
TriggerReason evaluate_trigger(const BrainState &st, int tick,
                               const std::string &event_text,
                               const std::string &mission_sig,
                               bool client_enabled,
                               const LlmBrainParams &params);

// 纯函数：任务结构签名（missions 的 id/type/target + 已完成 id 列表拼接）。
// 任何一项变化都会改变签名 → 触发 MISSION_CHANGE。不做事件文本解析。
std::string mission_signature(const Perception &p);

// 纯函数：system 提示词（默认稿；{W}/{H} 代入地图尺寸）
std::string build_system_message(int map_w, int map_h);

// 纯函数：user 消息（任务原文+结构 / 本车状态 / 事件公告原文整段嵌入）
std::string build_user_message(const Perception &p);

// 每帧驱动的 LLM 触发与结论持有者（bot_node 与 Strategy 之间的管道）
class LlmBrain {
 public:
  // 生产构造：LlmClient 全部配置来自环境变量（见 llm_client.hpp 文件头）
  LlmBrain();
  // 自检/注入构造：外部给客户端与参数
  explicit LlmBrain(std::shared_ptr<LlmClient> client,
                    LlmBrainParams params = LlmBrainParams{});

  // 每帧调用：先消费客户端最新完成结果（失败→退避），再判定触发，
  // 触发则构造 prompt 并入队。绝不阻塞（submit 只入队）。
  void observe(const Perception &p);

  // 最近一次完成的 LlmResult（脑内缓存副本，observe 时随客户端更新；
  // 可能 DISABLED / 失败态——Strategy 使用前自行判断 status，
  // 并用 llm_conclusion.h 解析 content）
  const LlmResult *latest_result() const { return &cached_result_; }

  // 换场回调：清空触发状态（首帧重新触发 SESSION_START）与缓存的结论副本
  // （M5 修复：新场首帧不得再读到上一场的 LLM 结论）
  void on_new_session(const std::string &session_id);

  // 统计与诊断（日志/资格核对用）
  int submitted() const { return st_.submitted; }
  bool client_enabled() const;   // 客户端已配置且未永久降级
  bool quota_degraded() const;
  const char *last_trigger() const;  // 最近一次触发原因名（日志用）

 private:
  // 调用失败类状态（需要退避的）：超时/网络/网关错/限流重试后仍败/解析失败。
  static bool is_failure_status(LlmResult::Status s);

  std::shared_ptr<LlmClient> client_;
  LlmBrainParams params_;
  BrainState st_;
  std::uint64_t last_done_count_ = 0;  // 客户端已完成调用数（识别"新"结果用）
  LlmResult cached_result_;            // 最近完成结果副本（latest_result 返回）
  std::string last_prompt_;            // 上一次提交的完整 prompt（去重用）
  TriggerReason last_trigger_ = TriggerReason::NONE;
};

}  // namespace rmtc_bot
