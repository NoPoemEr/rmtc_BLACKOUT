// strategy_selftest.cpp —— 策略接口与配置加载自检（不依赖 ROS）
//
// 用法：ros2 run rmtc_bot strategy_selftest
// 覆盖：
//   1. ActionChoice 便捷构造（wait/move/attack/defend 字段）
//   2. WaitStrategy：任何输入都返回 wait；换场回调不崩溃
//   3. 配置加载：wait 档生效；collect 档已登记（占位实现，动作逻辑由
//      collect_selftest 验收）；combat 档已登记（占位实现，规则层由
//      combat_selftest 验收）；未知档回退 wait
//   4. 配置异常：文件不存在 / 非法 JSON / 缺字段 → 全部回退 wait（不崩溃）

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include "rmtc_bot/strategy.hpp"

using rmtc_bot::ActionChoice;
using rmtc_bot::LlmResult;
using rmtc_bot::make_strategy;
using rmtc_bot::Perception;
using rmtc_bot::Strategy;

namespace {

int g_fail = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
      ++g_fail;                                                            \
    }                                                                      \
    std::fflush(stdout);                                                   \
  } while (0)

// 写一个临时配置文件并把它设为 RMTC_DECISION_CONFIG。
// S5 修复：用 mkstemp 拿独占临时文件（不再固定 /tmp 路径），并断言写成功。
std::string g_config_path;

void SetConfig(const std::string &content) {
  char tmpl[] = "/tmp/rmtc_decision_config_test_XXXXXX";
  const int fd = ::mkstemp(tmpl);
  if (fd < 0) {
    std::fprintf(stderr, "[selftest] mkstemp 失败\n");
    std::abort();
  }
  ::close(fd);
  g_config_path = tmpl;
  std::ofstream f(g_config_path);
  if (!f.is_open()) {
    std::fprintf(stderr, "[selftest] 写配置失败: %s\n", g_config_path.c_str());
    std::abort();
  }
  f << content;
  f.close();
  ::setenv("RMTC_DECISION_CONFIG", g_config_path.c_str(), 1);
}

void TestActionChoice() {
  std::printf("[TEST] ActionChoice 便捷构造\n");
  ActionChoice w = ActionChoice::Wait("r");
  CHECK(w.action == "wait" && w.direction.empty());

  ActionChoice m = ActionChoice::Move("right", "r");
  CHECK(m.action == "move" && m.direction == "right");

  ActionChoice a = ActionChoice::Attack("up", "r");
  CHECK(a.action == "attack" && a.direction == "up");

  ActionChoice d = ActionChoice::Defend("r");
  CHECK(d.action == "defend" && d.direction.empty());
}

void TestWaitStrategy() {
  std::printf("[TEST] WaitStrategy 行为\n");
  Perception p;  // 空感知（未解析）也不该让策略崩溃
  LlmResult llm;
  rmtc_bot::WaitStrategy s;
  ActionChoice c = s.decide(p, &llm);
  CHECK(c.action == "wait");
  CHECK(std::string(s.name()) == "wait");
  s.on_new_session("some-session");  // 换场回调不崩溃即可
}

void TestConfigLoad() {
  std::printf("[TEST] 配置加载（各档位与回退）\n");

  // 1) wait 档：正常生效
  SetConfig("{\"decision_type\": \"wait\"}");
  {
    std::unique_ptr<Strategy> s = make_strategy();
    CHECK(std::string(s->name()) == "wait");
  }

  // 2) collect：已登记（10/20，占位永远 wait；10/25 填充后由 collect_selftest 验收）
  SetConfig("{\"decision_type\": \"collect\"}");
  {
    std::unique_ptr<Strategy> s = make_strategy();
    CHECK(std::string(s->name()) == "collect");
  }

  // 2b) combat：已登记（10/21，占位永远 wait；10/27 填充后由 combat_selftest 验收）
  SetConfig("{\"decision_type\": \"combat\"}");
  {
    std::unique_ptr<Strategy> s = make_strategy();
    CHECK(std::string(s->name()) == "combat");
  }

  // 3) 未知档位：回退 wait
  SetConfig("{\"decision_type\": \"yolo\"}");
  {
    std::unique_ptr<Strategy> s = make_strategy();
    CHECK(std::string(s->name()) == "wait");
  }

  // 4) 文件不存在：回退 wait
  ::setenv("RMTC_DECISION_CONFIG", "/tmp/does_not_exist_12345.json", 1);
  {
    std::unique_ptr<Strategy> s = make_strategy();
    CHECK(std::string(s->name()) == "wait");
  }

  // 5) 非法 JSON：回退 wait
  SetConfig("{{{ 这不是 JSON");
  {
    std::unique_ptr<Strategy> s = make_strategy();
    CHECK(std::string(s->name()) == "wait");
  }

  // 6) 缺字段：回退 wait
  SetConfig("{\"other\": 1}");
  {
    std::unique_ptr<Strategy> s = make_strategy();
    CHECK(std::string(s->name()) == "wait");
  }

  // 清理测试环境变量
  ::unsetenv("RMTC_DECISION_CONFIG");
}

}  // namespace

int main() {
  std::printf("==== 策略接口与配置加载自检 ====\n");
  TestActionChoice();
  TestWaitStrategy();
  TestConfigLoad();

  if (g_fail == 0) {
    std::printf("==== 全部通过 ====\n");
    return 0;
  }
  std::printf("==== 失败 %d 项 ====\n", g_fail);
  return 1;
}
