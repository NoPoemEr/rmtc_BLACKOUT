// llm_selftest.cpp —— LLM 客户端自检（不依赖 ROS，可单独运行）
//
// 用法：ros2 run rmtc_bot llm_selftest   （或直接运行 install/lib/rmtc_bot/llm_selftest）
// 退出码：0 = 全部通过；1 = 有用例失败。
//
// 思路：文件内建一个极简 HTTP 模拟网关（MockServer），按测试场景返回
// 不同的状态码/正文/行为（含"收完请求但不回话"的超时场景），
// 从而在没有任何外部服务的情况下验证：
//   1. 请求报文格式（POST 路径、Bearer 鉴权、Content-Length 正确、无 Transfer-Encoding）
//   2. 成功响应的解析（choices[0].message.content）
//   3. 协议错误码表映射（401/429 quota/429 rate_limit/503…）
//   4. 非 JSON 正文、缺字段正文的容错
//   5. 连接拒绝与读超时
//   6. 异步接口：submit 不阻塞、结果按序完成、统计正确
//   7. 429 quota_exceeded 永久降级后拒绝新任务
//   8. 环境变量配置读取与"缺配置禁用"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "rmtc_bot/llm_client.hpp"

using rmtc_bot::LlmClient;
using rmtc_bot::LlmResult;

namespace {

// ---- 极简 HTTP 模拟网关 -----------------------------------------------------
// 监听 127.0.0.1 随机端口；接受 max_conns 个连接；handler 决定响应。
class MockServer {
 public:
  enum class Mode { RESPOND, HOLD };  // HOLD：读请求后不回话（超时测试）

  using Handler = std::function<std::string(const std::string &head,
                                            const std::string &body)>;

  MockServer(int max_conns, Mode mode, Handler handler)
      : max_conns_(max_conns), mode_(mode), handler_(std::move(handler)) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // 系统自动分配端口
    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
        ::listen(listen_fd_, 8) < 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread(&MockServer::run, this);
  }

  ~MockServer() {
    // Linux 上 close(listen_fd) 不会打断另一个线程的阻塞 accept，
    // 所以先发一个"唤醒连接"，保证线程一定能退出（避免测试因连接数不符而永久挂起）
    stop_.store(true);
    if (listen_fd_ >= 0) {
      int w = ::socket(AF_INET, SOCK_STREAM, 0);
      if (w >= 0) {
        struct sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<std::uint16_t>(port_));
        ::connect(w, reinterpret_cast<sockaddr *>(&a), sizeof(a));
        ::close(w);
      }
      ::close(listen_fd_);
    }
    if (thread_.joinable()) thread_.join();
  }

  int port() const { return port_; }
  bool ok() const { return listen_fd_ >= 0; }

 private:
  void run() {
    for (int i = 0; i < max_conns_ && !stop_.load(); ++i) {
      int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) break;
      if (stop_.load()) { ::close(fd); break; }
      // 读请求：先读到头部结束，再按 Content-Length 读正文
      std::string buf;
      char tmp[8192];
      std::size_t end = std::string::npos;
      while (end == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<std::size_t>(n));
        end = buf.find("\r\n\r\n");
      }
      if (end == std::string::npos) { ::close(fd); continue; }
      std::size_t clen = 0;
      std::size_t p = buf.find("Content-Length:");
      if (p != std::string::npos) clen = static_cast<std::size_t>(std::atol(buf.c_str() + p + 15));
      while (buf.size() < end + 4 + clen) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<std::size_t>(n));
      }
      if (mode_ == Mode::RESPOND) {
        std::string resp = handler_(buf.substr(0, end), buf.substr(end + 4, clen));
        ::send(fd, resp.data(), resp.size(), 0);
        ::close(fd);
      } else {
        // HOLD：保持连接不回话，让客户端读超时；随后关闭
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        ::close(fd);
      }
    }
  }

  int listen_fd_ = -1;
  int port_ = -1;
  int max_conns_;
  Mode mode_;
  Handler handler_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
};

// 组装一个完整 HTTP 响应（我们只测客户端，不需要真实头）
std::string HttpResponse(int code, const std::string &body) {
  std::string s = "HTTP/1.1 " + std::to_string(code) + " Mock\r\n";
  s += "Content-Type: application/json\r\n";
  s += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  s += "Connection: close\r\n\r\n";
  s += body;
  return s;
}

// 组装一个成功的 Chat Completions 响应正文
std::string OkBody(const std::string &content) {
  nlohmann::json j;
  j["choices"] = nlohmann::json::array();
  j["choices"].push_back({{"message", {{"content", content}}}, {"finish_reason", "stop"}});
  return j.dump();
}

// 组装一个网关错误响应正文
std::string ErrorBody(const std::string &code) {
  nlohmann::json j;
  j["error"] = {{"message", "mock"}, {"type", "mock"}, {"param", nullptr}, {"code", code}};
  return j.dump();
}

// ---- 测试框架（极简） -------------------------------------------------------
int g_fail = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++g_fail;                                                              \
    }                                                                        \
    std::fflush(stdout);                                                     \
  } while (0)

void Begin(const char *name) {
  std::printf("[TEST] %s\n", name);
  std::fflush(stdout);
}

// ---- 测试用例 -----------------------------------------------------------------

// 用例 1：请求报文格式逐字节核对 + 成功解析
void TestWireFormat() {
  Begin("请求报文格式（POST 路径/鉴权/Content-Length/无 Transfer-Encoding）");
  std::string captured_head, captured_body;
  MockServer server(5, MockServer::Mode::RESPOND,
                    [&](const std::string &head, const std::string &body) {
                      captured_head = head;
                      captured_body = body;
                      return HttpResponse(200, OkBody("hello from mock"));
                    });
  CHECK(server.ok());

  // 根地址构造 → 自动补全 /v1/chat/completions 路径
  LlmClient client("http://127.0.0.1:" + std::to_string(server.port()), "practice-A");
  LlmResult r = client.call("测试提示词 {x:1}");
  CHECK(r.status == LlmResult::Status::SUCCESS);
  CHECK(r.content == "hello from mock");
  CHECK(r.http_code == 200);

  // 报文核对
  CHECK(captured_head.find("POST /v1/chat/completions HTTP/1.1\r\n") == 0);
  CHECK(captured_head.find("Host: 127.0.0.1:") != std::string::npos);
  CHECK(captured_head.find("Content-Type: application/json\r\n") != std::string::npos);
  CHECK(captured_head.find("Authorization: Bearer practice-A\r\n") != std::string::npos);
  CHECK(captured_head.find("Transfer-Encoding") == std::string::npos);  // 网关对带该头的请求返回 501
  std::string expect_len = "Content-Length: " + std::to_string(captured_body.size());
  CHECK(captured_head.find(expect_len) != std::string::npos);

  nlohmann::json body = nlohmann::json::parse(captured_body);
  CHECK(body["stream"] == false);
  CHECK(body["messages"].is_array() && body["messages"].size() == 1);
  CHECK(body["messages"][0]["role"] == "user");
  CHECK(body["messages"][0]["content"] == "测试提示词 {x:1}");
  CHECK(body["max_tokens"] == 512);
  CHECK(body["temperature"].is_number() && body["temperature"].get<double>() == 0.2);
  CHECK(body["response_format"].is_object() &&
        body["response_format"]["type"] == "json_object");
  CHECK(!body.contains("model"));  // 未配置模型名则不发送该字段
  // 思考模式默认关闭（DeepSeek 系思考模式默认开启会拖慢首字→超时）
  CHECK(body["thinking"].is_object() && body["thinking"]["type"] == "disabled");

  // response_format=none → 不发送该字段（现场逃生门）
  {
    LlmClient client2("http://127.0.0.1:" + std::to_string(server.port()), "practice-A",
                      /*model=*/"", /*max_tokens=*/512, /*timeout_ms=*/6000,
                      /*max_retries=*/2, /*retry_backoff_ms=*/1000,
                      /*temperature=*/0.2, /*response_format=*/"none");
    LlmResult r2 = client2.call("p");
    CHECK(r2.status == LlmResult::Status::SUCCESS);
    nlohmann::json body2 = nlohmann::json::parse(captured_body);
    CHECK(!body2.contains("response_format"));
    CHECK(body2["temperature"].get<double>() == 0.2);
  }

  // thinking=off → 不发送该字段（兼容不支持 thinking 的网关）
  {
    LlmClient client3("http://127.0.0.1:" + std::to_string(server.port()), "practice-A",
                      /*model=*/"", /*max_tokens=*/512, /*timeout_ms=*/6000,
                      /*max_retries=*/2, /*retry_backoff_ms=*/1000,
                      /*temperature=*/0.2, /*response_format=*/"json_object",
                      /*thinking=*/"off");
    LlmResult r3 = client3.call("p");
    CHECK(r3.status == LlmResult::Status::SUCCESS);
    nlohmann::json body3 = nlohmann::json::parse(captured_body);
    CHECK(!body3.contains("thinking"));
  }

  // thinking=enabled → 显式开启（调试/特殊网关用）
  {
    LlmClient client4("http://127.0.0.1:" + std::to_string(server.port()), "practice-A",
                      /*model=*/"", /*max_tokens=*/512, /*timeout_ms=*/6000,
                      /*max_retries=*/2, /*retry_backoff_ms=*/1000,
                      /*temperature=*/0.2, /*response_format=*/"json_object",
                      /*thinking=*/"enabled");
    LlmResult r4 = client4.call("p");
    CHECK(r4.status == LlmResult::Status::SUCCESS);
    nlohmann::json body4 = nlohmann::json::parse(captured_body);
    CHECK(body4["thinking"].is_object() && body4["thinking"]["type"] == "enabled");
  }

  // 显式 model → 发送 model 字段
  {
    LlmClient client5("http://127.0.0.1:" + std::to_string(server.port()), "practice-A",
                      /*model=*/"deepseek-flash");
    LlmResult r5 = client5.call("p");
    CHECK(r5.status == LlmResult::Status::SUCCESS);
    nlohmann::json body5 = nlohmann::json::parse(captured_body);
    CHECK(body5["model"] == "deepseek-flash");
  }
}

// 用例 2：错误码表映射（协议 §5 本地错误码）
void TestErrorMapping() {
  Begin("协议错误码映射");
  {
    MockServer server(1, MockServer::Mode::RESPOND,
                      [](const std::string &, const std::string &) {
                        return HttpResponse(429, ErrorBody("quota_exceeded"));
                      });
    LlmClient client("http://127.0.0.1:" + std::to_string(server.port()), "tok");
    LlmResult r = client.call("p");
    CHECK(r.status == LlmResult::Status::QUOTA_EXHAUSTED);
    CHECK(r.error_code == "quota_exceeded");
    CHECK(r.http_code == 429);
  }
  {
    MockServer server(1, MockServer::Mode::RESPOND,
                      [](const std::string &, const std::string &) {
                        return HttpResponse(401, ErrorBody("invalid_token"));
                      });
    LlmClient client("http://127.0.0.1:" + std::to_string(server.port()), "tok");
    LlmResult r = client.call("p");
    CHECK(r.status == LlmResult::Status::AUTH_ERROR);
    CHECK(r.error_code == "invalid_token");
  }
  {
    MockServer server(1, MockServer::Mode::RESPOND,
                      [](const std::string &, const std::string &) {
                        return HttpResponse(503, ErrorBody("audit_unavailable"));
                      });
    LlmClient client("http://127.0.0.1:" + std::to_string(server.port()), "tok");
    LlmResult r = client.call("p");
    CHECK(r.status == LlmResult::Status::HTTP_ERROR);
    CHECK(r.error_code == "audit_unavailable");
  }
}

// 用例 3：429 rate_limit → 退避重试后仍失败（重试 2 次共 3 次请求）
void TestRateLimitRetry() {
  Begin("429 rate_limit 退避重试");
  MockServer server(3, MockServer::Mode::RESPOND,
                    [](const std::string &, const std::string &) {
                      return HttpResponse(429, ErrorBody("rate_limit"));
                    });
  // 重试 2 次、退避 50ms（测试用小值）
  LlmClient client("http://127.0.0.1:" + std::to_string(server.port()), "tok",
                   "", 300, 2000, 2, 50);
  LlmResult r = client.call("p");
  CHECK(r.status == LlmResult::Status::RATE_LIMITED);
  CHECK(r.attempts == 3);
  CHECK(r.error_code == "rate_limit");
}

// 用例 4：2xx 但正文不是合法 Chat Completions → 解析错误，不崩溃
void TestParseErrors() {
  Begin("2xx 非 JSON / 缺字段正文容错");
  {
    MockServer server(1, MockServer::Mode::RESPOND,
                      [](const std::string &, const std::string &) {
                        return HttpResponse(200, "这不是 JSON");
                      });
    LlmClient client("http://127.0.0.1:" + std::to_string(server.port()), "tok");
    LlmResult r = client.call("p");
    CHECK(r.status == LlmResult::Status::PARSE_ERROR);
  }
  {
    MockServer server(1, MockServer::Mode::RESPOND,
                      [](const std::string &, const std::string &) {
                        return HttpResponse(200, "{\"choices\":[]}");
                      });
    LlmClient client("http://127.0.0.1:" + std::to_string(server.port()), "tok");
    LlmResult r = client.call("p");
    CHECK(r.status == LlmResult::Status::PARSE_ERROR);
  }
}

// 用例 5：连接拒绝 → NETWORK_ERROR（不崩溃）
void TestConnectionRefused() {
  Begin("连接拒绝");
  // 先绑定再关闭，得到一个"确实无人监听"的端口
  int probe = ::socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(probe, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(probe, reinterpret_cast<sockaddr *>(&addr), &len);
  int dead_port = ntohs(addr.sin_port);
  ::close(probe);

  LlmClient client("http://127.0.0.1:" + std::to_string(dead_port), "tok",
                   "", 300, 1000);
  LlmResult r = client.call("p");
  CHECK(r.status == LlmResult::Status::NETWORK_ERROR);
  CHECK(r.http_code == 0);
}

// 用例 6：读超时（服务端不回话）
void TestTimeout() {
  Begin("读超时（300ms）");
  MockServer server(1, MockServer::Mode::HOLD,
                    [](const std::string &, const std::string &) { return std::string(); });
  LlmClient client("http://127.0.0.1:" + std::to_string(server.port()), "tok",
                   "", 300, 300, 0);
  LlmResult r = client.call("p");
  CHECK(r.status == LlmResult::Status::TIMEOUT);
}

// 用例 7：异步接口——submit 不阻塞、按序完成、统计正确
void TestAsync() {
  Begin("异步 submit/latest_result/统计");
  int conn = 0;
  MockServer server(2, MockServer::Mode::RESPOND,
                    [&](const std::string &, const std::string &) {
                      ++conn;
                      return HttpResponse(200, OkBody(conn == 1 ? "answer-1" : "answer-2"));
                    });
  LlmClient client("http://127.0.0.1:" + std::to_string(server.port()), "tok");

  // submit 必须立刻返回（后台线程还没拿到结果时也不阻塞）
  auto t0 = std::chrono::steady_clock::now();
  CHECK(client.submit("问题 1"));
  CHECK(client.submit("问题 2"));
  auto submit_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
  CHECK(submit_ms < 100.0);  // 两次提交总共应在 100ms 内返回

  // 等待后台处理完两条（最多等 5s）
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (client.done_count() < 2 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(client.done_count() == 2);
  CHECK(client.success_count() == 2);
  CHECK(client.total_submitted() == 2);
  CHECK(client.has_result());
  CHECK(client.latest_result().status == LlmResult::Status::SUCCESS);
  CHECK(client.latest_result().content == "answer-2");  // 最近一次完成的结论
}

// 用例 8：quota_exceeded 永久降级 → 拒绝新任务（本场不再浪费请求）
void TestQuotaDegrade() {
  Begin("429 quota_exceeded 永久降级");
  MockServer server(1, MockServer::Mode::RESPOND,
                    [](const std::string &, const std::string &) {
                      return HttpResponse(429, ErrorBody("quota_exceeded"));
                    });
  LlmClient client("http://127.0.0.1:" + std::to_string(server.port()), "tok");

  CHECK(client.submit("p1"));
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (client.done_count() < 1 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(client.done_count() == 1);
  CHECK(client.quota_degraded());
  CHECK(!client.enabled());
  CHECK(!client.submit("p2"));        // 降级后拒绝新任务
  CHECK(client.total_submitted() == 1);  // 统计不增加
  CHECK(client.latest_result().status == LlmResult::Status::QUOTA_EXHAUSTED);
}

// 用例 9：环境变量配置读取 + 缺配置禁用
void TestEnvConfig() {
  Begin("环境变量配置");
  ::setenv("RMTC_LLM_URL", "http://192.168.1.9:8000", 1);
  ::setenv("RMTC_TEAM_TOKEN", "env-token", 1);
  ::setenv("RMTC_LLM_MODEL", "env-model", 1);
  ::setenv("RMTC_LLM_THINKING", "off", 1);
  ::setenv("RMTC_LLM_MAX_TOKENS", "512", 1);
  ::setenv("RMTC_LLM_TEMPERATURE", "0.35", 1);
  ::setenv("RMTC_LLM_RESPONSE_FORMAT", "none", 1);
  ::setenv("RMTC_LLM_TIMEOUT_MS", "4500", 1);

  LlmClient client;
  CHECK(client.enabled());
  CHECK(client.url() == "http://192.168.1.9:8000");
  CHECK(client.model() == "env-model");
  CHECK(client.thinking() == "off");
  CHECK(client.max_tokens() == 512);
  CHECK(client.temperature() == 0.35);
  CHECK(client.response_format() == "none");
  CHECK(client.timeout_ms() == 4500);

  ::unsetenv("RMTC_LLM_URL");
  ::unsetenv("RMTC_TEAM_TOKEN");
  ::unsetenv("RMTC_LLM_MODEL");
  ::unsetenv("RMTC_LLM_THINKING");
  ::unsetenv("RMTC_LLM_MAX_TOKENS");
  ::unsetenv("RMTC_LLM_TEMPERATURE");
  ::unsetenv("RMTC_LLM_RESPONSE_FORMAT");
  ::unsetenv("RMTC_LLM_TIMEOUT_MS");

  LlmClient no_config;
  CHECK(!no_config.enabled());      // 缺地址/令牌 → 禁用
  CHECK(!no_config.submit("p"));    // 提交被拒绝
  LlmResult r = no_config.call("p");
  CHECK(r.status == LlmResult::Status::DISABLED);
  // 未配置时的默认值：模型显式 deepseek-flash、思考模式默认关闭
  CHECK(no_config.model() == "deepseek-flash");
  CHECK(no_config.thinking() == "disabled");
}

// 用例 10：非法 URL 不崩溃（禁用处理）
void TestBadUrl() {
  Begin("非法 URL 容错");
  LlmClient client("ftp://nonsense", "tok");
  CHECK(!client.enabled());
  LlmResult r = client.call("p");
  CHECK(r.status == LlmResult::Status::DISABLED);
}

}  // namespace

int main() {
  std::printf("==== LLM 客户端自检 ====\n");
  TestWireFormat();
  TestErrorMapping();
  TestRateLimitRetry();
  TestParseErrors();
  TestConnectionRefused();
  TestTimeout();
  TestAsync();
  TestQuotaDegrade();
  TestEnvConfig();
  TestBadUrl();

  if (g_fail == 0) {
    std::printf("==== 全部通过 ====\n");
    return 0;
  }
  std::printf("==== 失败 %d 项 ====\n", g_fail);
  return 1;
}
