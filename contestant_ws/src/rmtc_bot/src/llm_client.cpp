// llm_client.cpp —— LLM 网关客户端实现
// 只依赖 POSIX socket + nlohmann/json，不依赖 libcurl 等外部库，随包自包含。

#include "rmtc_bot/llm_client.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

namespace rmtc_bot {

namespace {

// 请求体上限 2 MiB（协议规定，超过网关返回 413；我们本地先拦一道）
constexpr std::size_t kMaxRequestBody = 2097152;
// 响应正文上限：模型输出只是固定 JSON 结论，4 MiB 足够且防异常流量
constexpr std::size_t kMaxResponseBody = 4 * 1024 * 1024;
// 响应头上限：正常网关响应头只有几百字节
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

// 解析 URL：http://host[:port][/path]。
// 只支持 http（正式网关是内网 http；练习也走本地 http 代理，https 由裁判代转）。
// 返回 false 表示格式不合法。
bool ParseUrl(const std::string &url, std::string *host, std::string *port,
              std::string *path) {
  const std::string prefix = "http://";
  if (url.compare(0, prefix.size(), prefix) != 0) return false;
  std::string rest = url.substr(prefix.size());
  // host[:port][/path]
  std::size_t slash = rest.find('/');
  std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  *path = (slash == std::string::npos) ? "/v1/chat/completions" : rest.substr(slash);
  // 尾斜杠（http://host/）→ 归一化为默认路径，避免 path="/" 全部 404（L12）
  if (path->empty() || *path == "/") *path = "/v1/chat/completions";
  if (authority.empty()) return false;
  // host 与 port 的切分：找最后一个 ':'（IPv6 场景先不管，内网用 IPv4/IP 直填）
  std::size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    *host = authority.substr(0, colon);
    *port = authority.substr(colon + 1);
  } else {
    *host = authority;
    *port = "80";
  }
  if (host->empty() || port->empty()) return false;
  // 端口必须是纯数字（L10：非数字端口 = 配置错误，视为 URL 非法 → 客户端禁用）
  for (const char c : *port) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// 建立连接（带超时）。成功返回 socket fd，失败返回 -1。
// 做法：非阻塞 connect + select 等可写 + SO_ERROR 确认，成功后恢复阻塞。
// 失败时 errno 语义：ETIMEDOUT=超时；其余=网络类错误（调用方据此分类）。
int ConnectWithTimeout(const std::string &host, const std::string &port,
                       int timeout_ms) {
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *result = nullptr;
  int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
  if (rc != 0 || result == nullptr) {
    errno = EHOSTUNREACH;  // 显式置网络类错误，防残留 EAGAIN 被误判为超时
    return -1;
  }

  int sock = -1;
  for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
    sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) continue;

    // 先设为非阻塞，connect 之后用 select 等待
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    rc = ::connect(sock, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(sock, &wfds);
      struct timeval tv;
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      rc = select(sock + 1, nullptr, &wfds, nullptr, &tv);
      if (rc > 0) {
        // 可写后还需确认没有连接错误
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error == 0) rc = 0;  // 连接成功
        else { errno = so_error; rc = -1; }
      } else if (rc == 0) {
        errno = ETIMEDOUT;  // select 超时
        rc = -1;
      }
    }
    if (rc == 0) break;  // 连接成功，跳出地址遍历
    ::close(sock);
    sock = -1;
  }
  freeaddrinfo(result);
  if (sock < 0) {
    // 地址遍历耗尽：清掉可能残留的 EAGAIN/EINPROGRESS 语义，统一按网络错误
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS) {
      errno = ECONNREFUSED;
    }
    return -1;
  }

  // 恢复阻塞模式，并设置收发超时（超时后 recv/send 返回 -1，errno=EAGAIN）
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) & ~O_NONBLOCK);
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return sock;
}

// 完整发送 data。返回 0=成功；-1=失败（连接关闭/网络错误）；
// -2=超时（errno EAGAIN/EWOULDBLOCK，SO_SNDTIMEO 触发）。
// MSG_NOSIGNAL（10/27 审查 H2 修复）：对已断开连接写不再触发 SIGPIPE
// 杀死整个 ROS 节点进程，而是返回 EPIPE 走错误分支。
int SendAll(int sock, const std::string &data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n =
        ::send(sock, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2;
      return -1;  // n==0 连接关闭；n<0 错误
    }
    sent += static_cast<std::size_t>(n);
  }
  return 0;
}

// 读取响应：先读到 "\r\n\r\n" 结束的头部，再按 Content-Length 读正文（无该头则读到 EOF）。
// 返回 false 表示读失败/超限；*timed_out=true 表示超时（其余为网络错误/协议错误）。
bool ReceiveResponse(int sock, std::string *head, std::string *body,
                     bool *timed_out) {
  *timed_out = false;
  std::string buf;
  char tmp[8192];
  std::size_t hdr_end = std::string::npos;
  while (hdr_end == std::string::npos) {
    ssize_t n = ::recv(sock, tmp, sizeof(tmp), 0);
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) *timed_out = true;
      return false;  // 0=EOF（没等来头部结束），<0=错误或超时
    }
    buf.append(tmp, static_cast<std::size_t>(n));
    hdr_end = buf.find("\r\n\r\n");
    if (hdr_end == std::string::npos && buf.size() > kMaxHeaderBytes) return false;
  }
  *head = buf.substr(0, hdr_end);
  std::string leftover = buf.substr(hdr_end + 4);  // 头部之后可能已带部分正文

  // 找 Content-Length（大小写不敏感）
  long content_length = -1;
  std::string head_lower = *head;
  for (auto &c : head_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::size_t pos = head_lower.find("content-length:");
  if (pos != std::string::npos) {
    content_length = std::atol(head->c_str() + pos + 15);
  }

  if (content_length >= 0) {
    if (static_cast<std::size_t>(content_length) > kMaxResponseBody) return false;
    std::string acc = leftover;
    while (acc.size() < static_cast<std::size_t>(content_length)) {
      ssize_t n = ::recv(sock, tmp, sizeof(tmp), 0);
      if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) *timed_out = true;
        return false;
      }
      acc.append(tmp, static_cast<std::size_t>(n));
    }
    *body = acc.substr(0, static_cast<std::size_t>(content_length));
  } else {
    // 没有 Content-Length：依赖 Connection: close，读到 EOF
    std::string acc = leftover;
    while (true) {
      ssize_t n = ::recv(sock, tmp, sizeof(tmp), 0);
      if (n == 0) break;  // EOF，收完
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) *timed_out = true;
        return false;
      }
      acc.append(tmp, static_cast<std::size_t>(n));
      if (acc.size() > kMaxResponseBody) return false;
    }
    *body = acc;
  }
  return true;
}

// 解析状态行（第一行 "HTTP/1.1 200 OK"）里的状态码；失败返回 -1
int ParseStatusCode(const std::string &head) {
  // 找 "HTTP/1." 开头，再跳过版本号后的空格
  if (head.rfind("HTTP/1.", 0) != 0) return -1;
  std::size_t sp = head.find(' ');
  if (sp == std::string::npos) return -1;
  return std::atoi(head.c_str() + sp + 1);
}

}  // namespace

LlmClient::LlmClient() {
  // 环境变量读取：练习/正式统一走这里，不硬编码
  const char *url = std::getenv("RMTC_LLM_URL");
  const char *token = std::getenv("RMTC_TEAM_TOKEN");
  const char *model = std::getenv("RMTC_LLM_MODEL");
  const char *thinking = std::getenv("RMTC_LLM_THINKING");
  const char *max_tokens = std::getenv("RMTC_LLM_MAX_TOKENS");
  const char *temperature = std::getenv("RMTC_LLM_TEMPERATURE");
  const char *response_format = std::getenv("RMTC_LLM_RESPONSE_FORMAT");
  const char *timeout = std::getenv("RMTC_LLM_TIMEOUT_MS");

  if (url != nullptr) url_ = url;
  if (token != nullptr) token_ = token;
  // 未设置时保持成员默认 deepseek-flash；显式置空则不发 model 字段（逃生门）
  if (model != nullptr) model_ = model;
  if (thinking != nullptr) {
    // 白名单校验：disabled/enabled 原样采用；off/none/空 = 不发送该字段；
    // 其余未知值告警并回退 off（宁可不发，也不发网关看不懂的值）
    const std::string t = thinking;
    if (t == "disabled" || t == "enabled" || t == "off" || t == "none" || t.empty()) {
      thinking_ = t;
    } else {
      std::fprintf(stderr,
                   "[llm_client] RMTC_LLM_THINKING 未知值（%s），按 off 处理"
                   "（不发送 thinking 字段）\n",
                   thinking);
      thinking_ = "off";
    }
  }
  if (max_tokens != nullptr) max_tokens_ = std::atoi(max_tokens);
  if (temperature != nullptr && temperature[0] != '\0') {
    temperature_ = std::strtod(temperature, nullptr);
  }
  if (response_format != nullptr) response_format_ = response_format;
  if (timeout != nullptr) timeout_ms_ = std::atoi(timeout);

  // L10 修复：环境变量数值校验——非法/非正值回退默认并告警，
  // 避免"配置了却全败"且无任何提示。
  if (max_tokens_ <= 0) {
    std::fprintf(stderr,
                 "[llm_client] RMTC_LLM_MAX_TOKENS 非法（%s），回退默认 512\n",
                 max_tokens != nullptr ? max_tokens : "(未设置)");
    max_tokens_ = 512;
  }
  if (timeout_ms_ <= 0) {
    std::fprintf(stderr,
                 "[llm_client] RMTC_LLM_TIMEOUT_MS 非法（%s），回退默认 6000\n",
                 timeout != nullptr ? timeout : "(未设置)");
    timeout_ms_ = 6000;
  }

  if (!url_.empty() && !ParseUrl(url_, &host_, &port_, &path_)) {
    // 地址格式非法 → 视为未配置（禁用），构造不崩溃
    host_.clear();
  }
}

LlmClient::LlmClient(const std::string &url, const std::string &token,
                     const std::string &model, int max_tokens, int timeout_ms,
                     int max_retries, int retry_backoff_ms,
                     double temperature, const std::string &response_format,
                     const std::string &thinking)
    : url_(url),
      token_(token),
      model_(model),
      thinking_(thinking),
      max_tokens_(max_tokens),
      temperature_(temperature),
      response_format_(response_format),
      timeout_ms_(timeout_ms),
      max_retries_(max_retries),
      retry_backoff_ms_(retry_backoff_ms) {
  if (!ParseUrl(url_, &host_, &port_, &path_)) host_.clear();
}

LlmClient::~LlmClient() { stop(); }

// ---------------------------------------------------------------------------
// 同步调用：attempt 次尝试；只有 RATE_LIMITED 才退避重试，其余立即返回
// ---------------------------------------------------------------------------
LlmResult LlmClient::call(const std::string &prompt) const {
  LlmResult final;
  if (!enabled()) return final;  // DISABLED

  for (int attempt = 1; attempt <= max_retries_ + 1; ++attempt) {
    LlmResult r = call_once(prompt);
    final.elapsed_ms += r.elapsed_ms;
    final.attempts = attempt;
    final.status = r.status;
    final.http_code = r.http_code;
    final.error_code = r.error_code;
    final.content = r.content;
    // RATE_LIMITED（频率/并发超限）→ 退避后重试；其余结果直接采用
    if (r.status == LlmResult::Status::RATE_LIMITED && attempt <= max_retries_) {
      if (stopping_.load()) break;  // 停机（M6）：放弃剩余退避重试，尽快退出
      std::this_thread::sleep_for(std::chrono::milliseconds(retry_backoff_ms_ * attempt));
      continue;
    }
    return final;
  }
  // 重试耗尽：保持最后一次尝试的 RATE_LIMITED 结果
  return final;
}

// ---------------------------------------------------------------------------
// 单次 HTTP 尝试（不含重试、不改共享状态）
// ---------------------------------------------------------------------------
LlmResult LlmClient::call_once(const std::string &prompt) const {
  LlmResult r;
  auto t0 = std::chrono::steady_clock::now();

  if (host_.empty() || token_.empty()) {
    r.status = LlmResult::Status::DISABLED;
    return r;
  }

  // 1) 组装请求体（OpenAI 兼容）
  nlohmann::json body;
  body["messages"] = nlohmann::json::array();
  body["messages"].push_back({{"role", "user"}, {"content", prompt}});
  body["stream"] = false;  // 非流式（网关只支持非流式）
  if (!model_.empty()) body["model"] = model_;
  body["max_tokens"] = max_tokens_;
  body["temperature"] = temperature_;  // 低温度 → 结论 JSON 更稳定（官方网关支持）
  if (!response_format_.empty() && response_format_ != "none") {
    body["response_format"] = {{"type", response_format_}};  // json_object 强制 JSON 输出
  }
  // 思考模式（默认关闭）：DeepSeek 系模型思考模式默认开启时首字延迟大、
  // 容易撞网关 5s 缓冲上限导致全超时；显式 {"type":"disabled"} 关闭。
  // off/none/空 = 不发送该字段（兼容不支持 thinking 的网关）。
  if (thinking_ == "disabled" || thinking_ == "enabled") {
    body["thinking"] = {{"type", thinking_}};
  }
  std::string body_str = body.dump();
  if (body_str.size() > kMaxRequestBody) {
    r.status = LlmResult::Status::HTTP_ERROR;
    r.error_code = "payload_too_large";  // 协议 413 场景，本地拦下
    return r;
  }

  // 2) 组装 HTTP 报文（注意：带 Content-Length、不带 Transfer-Encoding）
  std::string request = "POST " + path_ + " HTTP/1.1\r\n";
  request += "Host: " + host_ + ":" + port_ + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Authorization: Bearer " + token_ + "\r\n";
  request += "Content-Length: " + std::to_string(body_str.size()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += body_str;

  // 3) 连接（带超时）
  int sock = ConnectWithTimeout(host_, port_, timeout_ms_);
  if (sock < 0) {
    // 超时与网络错误靠 errno 分类（ConnectWithTimeout 已显式置 errno，L8）
    r.status = (errno == ETIMEDOUT) ? LlmResult::Status::TIMEOUT
                                    : LlmResult::Status::NETWORK_ERROR;
    r.elapsed_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
    return r;
  }

  // 4) 发送 → 接收
  const int send_rc = SendAll(sock, request);
  if (send_rc != 0) {
    ::close(sock);
    r.status = (send_rc == -2) ? LlmResult::Status::TIMEOUT
                               : LlmResult::Status::NETWORK_ERROR;
    r.elapsed_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
    return r;
  }
  std::string head, resp_body;
  bool rx_timed_out = false;
  if (!ReceiveResponse(sock, &head, &resp_body, &rx_timed_out)) {
    ::close(sock);
    // 读失败按 ReceiveResponse 的显式超时标记分类（L8：不再猜 errno 快照）
    r.status = rx_timed_out ? LlmResult::Status::TIMEOUT
                            : LlmResult::Status::NETWORK_ERROR;
    r.elapsed_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
    return r;
  }
  ::close(sock);
  r.elapsed_ms = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t0).count();

  int code = ParseStatusCode(head);
  r.http_code = code;

  // 5) 按状态码处理
  if (code >= 200 && code < 300) {
    // 成功：正文必须是 Chat Completions JSON
    try {
      nlohmann::json j = nlohmann::json::parse(resp_body);
      if (j.is_object() && j.contains("choices") && j["choices"].is_array() &&
          !j["choices"].empty() && j["choices"][0].contains("message") &&
          j["choices"][0]["message"].contains("content") &&
          j["choices"][0]["message"]["content"].is_string()) {
        r.status = LlmResult::Status::SUCCESS;
        r.content = j["choices"][0]["message"]["content"].get<std::string>();
      } else {
        r.status = LlmResult::Status::PARSE_ERROR;
      }
    } catch (const nlohmann::json::exception &) {
      r.status = LlmResult::Status::PARSE_ERROR;
    }
    return r;
  }

  // 非 2xx：尽量取出网关错误码（正文可能不是 JSON，逐层容错）
  r.status = LlmResult::Status::HTTP_ERROR;
  try {
    nlohmann::json j = nlohmann::json::parse(resp_body);
    if (j.is_object() && j.contains("error") && j["error"].is_object() &&
        j["error"].contains("code") && j["error"]["code"].is_string()) {
      r.error_code = j["error"]["code"].get<std::string>();
    }
  } catch (const nlohmann::json::exception &) {
    // 非 JSON 错误正文：保留 HTTP_ERROR，error_code 为空
  }

  if (code == 401) {
    r.status = LlmResult::Status::AUTH_ERROR;  // 令牌缺失/无效 → 永久降级
  } else if (code == 429) {
    if (r.error_code == "quota_exceeded") {
      r.status = LlmResult::Status::QUOTA_EXHAUSTED;  // 额度耗尽 → 永久降级
    } else {
      r.status = LlmResult::Status::RATE_LIMITED;  // 频率/并发 → 退避重试
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// 异步接口
// ---------------------------------------------------------------------------
void LlmClient::start() {
  std::lock_guard<std::mutex> lk(mu_);
  if (!started_) {
    started_ = true;
    running_.store(true);
    worker_ = std::thread(&LlmClient::worker_loop, this);
  }
}

void LlmClient::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stopping_.store(true);   // 在途调用的退避重试看到后立即放弃（M6）
    running_.store(false);
  }
  cv_.notify_all();
  // M6 修复：worker 见 running_=false 立即退出并丢弃剩余队列，
  // join 至多等"当前在途一条"（其退避重试也会被 stopping_ 打断），
  // 不再排空整队等全部超时。
  if (worker_.joinable()) worker_.join();
}

bool LlmClient::submit(const std::string &prompt) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!enabled_locked()) return false;      // 未配置或已永久降级（锁内用无锁版本）
  if (queue_.size() >= kMaxQueue) queue_.pop_front();  // 队满丢最旧
  LlmJob job;
  job.prompt = prompt;
  queue_.push_back(std::move(job));
  ++submitted_;
  if (!started_) {
    started_ = true;
    running_.store(true);
    worker_ = std::thread(&LlmClient::worker_loop, this);
  }
  cv_.notify_one();
  return true;
}

void LlmClient::worker_loop() {
  while (true) {
    LlmJob job;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return !running_.load() || !queue_.empty(); });
      // 停机（M6）：直接退出并丢弃剩余队列，不排空
      if (!running_.load()) break;
      job = queue_.front();
      queue_.pop_front();
    }
    // 网络调用在锁外执行，绝不影响 submit/统计的响应速度。
    // 异常兜底（L15）：任何调用异常都不允许 std::terminate 连带节点死亡。
    LlmResult r;
    try {
      r = call(job.prompt);
    } catch (const std::exception &e) {
      std::fprintf(stderr, "[llm_client] 调用异常：%s\n", e.what());
      r.status = LlmResult::Status::HTTP_ERROR;
      r.error_code = "internal_error";
    } catch (...) {
      std::fprintf(stderr, "[llm_client] 调用未知异常\n");
      r.status = LlmResult::Status::HTTP_ERROR;
      r.error_code = "internal_error";
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++done_;
      if (r.status == LlmResult::Status::SUCCESS) ++success_;
      last_result_ = r;
      has_result_ = true;
      if (r.status == LlmResult::Status::QUOTA_EXHAUSTED) quota_degraded_ = true;
      if (r.status == LlmResult::Status::AUTH_ERROR) auth_degraded_ = true;
    }
  }
}

LlmResult LlmClient::latest_result() const {
  std::lock_guard<std::mutex> lk(mu_);
  return last_result_;
}

bool LlmClient::has_result() const {
  std::lock_guard<std::mutex> lk(mu_);
  return has_result_;
}

bool LlmClient::enabled() const {
  std::lock_guard<std::mutex> lk(mu_);
  return enabled_locked();
}

bool LlmClient::enabled_locked() const {
  return !host_.empty() && !token_.empty() && !quota_degraded_ && !auth_degraded_;
}

bool LlmClient::quota_degraded() const {
  std::lock_guard<std::mutex> lk(mu_);
  return quota_degraded_;
}

bool LlmClient::auth_degraded() const {
  std::lock_guard<std::mutex> lk(mu_);
  return auth_degraded_;
}

std::uint64_t LlmClient::total_submitted() const {
  std::lock_guard<std::mutex> lk(mu_);
  return submitted_;
}

std::uint64_t LlmClient::done_count() const {
  std::lock_guard<std::mutex> lk(mu_);
  return done_;
}

std::uint64_t LlmClient::success_count() const {
  std::lock_guard<std::mutex> lk(mu_);
  return success_;
}

}  // namespace rmtc_bot
