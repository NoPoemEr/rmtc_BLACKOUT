#pragma once
// LLM 网关客户端（OpenAI 兼容 /v1/chat/completions）——纯"管道"模块，不含任何策略。
//
// 职责（与协议 §5「LLM 网关」一一对应）：
//   1. 异步调用：submit() 只入队、立刻返回，绝不阻塞动作窗口（协议禁止同步阻塞）
//   2. HTTP 细节：POST + Content-Length（不带 Transfer-Encoding，否则网关 501）、
//      Authorization: Bearer <令牌>、非流式（stream=false）
//   3. 错误码映射（协议本地错误码表）：
//      429 quota_exceeded      → 额度耗尽，永久降级（本场不再发请求）
//      429 rate_limit/并发超限 → 退避重试（默认重试 2 次，1s/2s 退避）
//      401 令牌缺失/无效       → 永久降级（令牌配置错误，重试无意义）
//      其余 HTTP/网络/解析错误 → 记入结果，由调用方决定怎么办
//   4. 统计成功次数：本场"AI 资格"要求 ≥1 次真实成功调用，success_count() 供核对
//
// 配置全部来自环境变量（不硬编码练习值，换正式配置零改动）：
//   RMTC_LLM_URL         完整端点或网关根地址（如 http://127.0.0.1:8080）
//                        —— 只支持 http（正式网关是内网 http；练习也走本地 http 代理）
//   RMTC_TEAM_TOKEN      本队/练习令牌（必填；缺失则整个客户端禁用）
//   RMTC_LLM_MODEL       模型名（默认 deepseek-flash；设为空字符串则显式不发送
//                        model 字段，改由主办方网关决定默认模型）
//   RMTC_LLM_THINKING    思考模式控制（默认 disabled，发送 {"type":"disabled"}，
//                         DeepSeek 思考模式默认开启会导致响应超时）；enabled=发送
//                        {"type":"enabled"}；off/none/空=不发送该字段（兼容不支持
//                        该字段的网关）；其余值告警并按 off 处理
//   RMTC_LLM_MAX_TOKENS  最大输出 token（默认 512；官方建议区间 512~1024，结论 JSON 有富余）
//   RMTC_LLM_TEMPERATURE 采样温度（默认 0.2，低温度让结论 JSON 更稳定；官方网关支持）
//   RMTC_LLM_RESPONSE_FORMAT 响应格式约束（默认 json_object；填 none 或空则不发送该字段）
//   RMTC_LLM_TIMEOUT_MS  连接/读超时毫秒（默认 6000；网关读超时是 5s，留 1s 余量）
//
// 线程模型：一个后台工作线程 + 一个任务队列（最多 16 条，队满丢最旧）。
// 同一时刻最多 1 个在途调用（网关允许每席位 2 个，留一半余量）。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace rmtc_bot {

// 一次调用的最终结果（同步 call() 与异步 latest_result() 共用）
struct LlmResult {
  enum class Status {
    SUCCESS,         // HTTP 2xx 且正文解析出 choices[0].message.content
    NETWORK_ERROR,   // 连接失败 / DNS 失败 / 发送失败
    TIMEOUT,         // 连接或读超时（网关只缓冲 5s，超时是常态，走本地降级即可）
    HTTP_ERROR,      // 非 2xx 且不属于下面几类（503/502/504 等）
    PARSE_ERROR,     // 2xx 但正文不是合法 JSON / 缺 choices / content 不是字符串
    QUOTA_EXHAUSTED, // 429 + quota_exceeded：本场额度耗尽 → 客户端永久降级
    RATE_LIMITED,    // 429 + rate_limit/concurrency_limit：退避重试后仍失败
    AUTH_ERROR,      // 401：令牌缺失/无效 → 客户端永久降级
    DISABLED,        // 未配置（无地址/无令牌）或已永久降级
  };

  Status status = Status::DISABLED;
  int http_code = 0;        // HTTP 状态码（网络错误时为 0）
  std::string error_code;   // 网关错误码原文（quota_exceeded 等），解析失败时为空
  std::string content;      // choices[0].message.content（成功时有效）
  double elapsed_ms = 0.0;  // 本次调用总耗时（含退避重试）
  int attempts = 0;         // 实际发起 HTTP 请求的次数
};

// 队列中的一条任务：只有一个 user 消息；提示词由调用方（策略）组好整段文本。
struct LlmJob {
  std::string prompt;
};

class LlmClient {
 public:
  // 从环境变量读取全部配置（见文件头说明）
  LlmClient();

  // 显式参数构造（自检/测试用；策略代码请用上面那个）
  LlmClient(const std::string &url, const std::string &token,
            const std::string &model = "", int max_tokens = 512,
            int timeout_ms = 6000, int max_retries = 2,
            int retry_backoff_ms = 1000, double temperature = 0.2,
            const std::string &response_format = "json_object",
            const std::string &thinking = "disabled");

  ~LlmClient();

  // ---- 异步接口（bot_node 的策略循环用这些）----

  // 入队一条调用，立刻返回。返回值：
  //   true  → 已入队（若队列已满，先丢弃最旧的一条再入队），后台线程会处理
  //   false → 客户端禁用/已永久降级（不执行也不入队）
  bool submit(const std::string &prompt);

  // 最近一次完成的调用结果（线程安全快照）。从未完成过时返回 DISABLED 空结果。
  LlmResult latest_result() const;
  bool has_result() const;

  // ---- 同步接口（自检、演示、调试用；策略循环禁止用，会阻塞动作窗口）----

  // 阻塞执行一次调用（含 RATE_LIMITED 退避重试），返回结果。不修改共享统计。
  LlmResult call(const std::string &prompt) const;

  // ---- 状态与统计（AI 资格核对用）----

  bool enabled() const;            // 已配置且未永久降级
  bool quota_degraded() const;     // 429 quota_exceeded 触发
  bool auth_degraded() const;      // 401 触发
  std::uint64_t total_submitted() const;
  std::uint64_t done_count() const;
  std::uint64_t success_count() const;  // 本场成功调用次数（≥1 才满足 AI 资格）

  // 诊断信息（自检用）
  const std::string &url() const { return url_; }
  const std::string &model() const { return model_; }
  const std::string &thinking() const { return thinking_; }
  int max_tokens() const { return max_tokens_; }
  double temperature() const { return temperature_; }
  const std::string &response_format() const { return response_format_; }
  int timeout_ms() const { return timeout_ms_; }

  // 生命周期：submit 会自动启动后台线程；stop() 停止并等待线程退出（析构自动
  // 调用）。停止时丢弃剩余队列、至多等当前在途一条完成（不排空，M6 修复）。
  void start();
  void stop();

 private:
  void worker_loop();  // 后台线程主循环：取任务 → 调用 → 更新统计

  // 单次 HTTP 尝试（不含重试），失败只返回结果、不改任何共享状态
  LlmResult call_once(const std::string &prompt) const;

  // enabled() 的"调用方已持有 mu_"版本（避免在锁内再上锁造成死锁）
  bool enabled_locked() const;

  // ---- 配置（构造后只读）----
  std::string url_;    // 完整端点（构造时由根地址补全）
  std::string host_;   // 解析后的主机名/IP
  std::string port_;   // 端口（默认 80）
  std::string path_;   // 路径（默认 /v1/chat/completions）
  std::string token_;
  std::string model_ = "deepseek-flash";  // 空 = 不发送 model 字段（环境变量显式置空）
  std::string thinking_ = "disabled";  // disabled/enabled=发送对应 thinking 字段；off/none/空=不发送
  int max_tokens_ = 512;
  double temperature_ = 0.2;
  std::string response_format_ = "json_object";  // none/空 = 不发送 response_format
  int timeout_ms_ = 6000;
  int max_retries_ = 2;
  int retry_backoff_ms_ = 1000;

  // ---- 多线程共享状态（全部用 mu_ 保护；running_ 是原子量，供 stop/重试路径无锁检查）----
  static constexpr std::size_t kMaxQueue = 16;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<LlmJob> queue_;
  std::thread worker_;
  std::atomic<bool> running_{false};  // 工作线程运行标志（M6：停机即弃队列）
  std::atomic<bool> stopping_{false};  // stop() 已请求（call() 据此放弃剩余退避重试）
  bool started_ = false;    // 工作线程是否已创建（只创建一次）
  LlmResult last_result_;   // 最近一次完成的调用结果
  bool has_result_ = false;
  std::uint64_t submitted_ = 0;
  std::uint64_t done_ = 0;
  std::uint64_t success_ = 0;
  bool quota_degraded_ = false;
  bool auth_degraded_ = false;
};

}  // namespace rmtc_bot
