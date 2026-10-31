// strategy.cpp —— 策略工厂与决策配置加载
//
// decision_config.json（现场只允许改 decision_type 这一个字段）：
//   {"decision_type": "wait"}     # 占位档：永远 wait（联调用）
//   {"decision_type": "collect"}  # 采集为主（10/25 已实现）
//   {"decision_type": "combat"}   # 对抗为主（10/27 已实现）
//
// 配置路径解析顺序：
//   1. 环境变量 RMTC_DECISION_CONFIG 指定的文件（run_contestant.sh 会导出包内默认路径）
//   2. 相对当前目录的 config/decision_config.json
//   3. 都不存在/解析失败 → 回退 "wait"（绝不因配置问题崩溃）

#include "rmtc_bot/collect_strategy.hpp"
#include "rmtc_bot/combat_strategy.hpp"
#include "rmtc_bot/strategy.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace rmtc_bot {

namespace {

// 读取 decision_type：任何异常都回退 "wait"
std::string load_decision_type() {
  const char *env = std::getenv("RMTC_DECISION_CONFIG");
  std::string path = (env != nullptr && env[0] != '\0')
                         ? env
                         : "config/decision_config.json";

  std::ifstream file(path);
  if (!file.is_open()) {
    std::fprintf(stderr, "[strategy] 配置 %s 不存在，使用默认档 wait\n",
                 path.c_str());
    return "wait";
  }
  try {
    nlohmann::json j = nlohmann::json::parse(file);
    if (j.is_object() && j.contains("decision_type") &&
        j["decision_type"].is_string()) {
      return j["decision_type"].get<std::string>();
    }
    std::fprintf(stderr, "[strategy] 配置缺少 decision_type，使用默认档 wait\n");
  } catch (const nlohmann::json::exception &e) {
    std::fprintf(stderr, "[strategy] 配置解析失败（%s），使用默认档 wait\n",
                 e.what());
  }
  return "wait";
}

}  // namespace

std::unique_ptr<Strategy> make_strategy() {
  std::string type = load_decision_type();

  if (type == "wait") {
    return std::make_unique<WaitStrategy>();
  }

  // CollectStrategy（10/25 已实现，由 collect_selftest 验收）
  if (type == "collect") {
    return std::make_unique<CollectStrategy>();
  }

  // CombatStrategy（10/27 已实现，由 combat_selftest 验收）
  if (type == "combat") {
    return std::make_unique<CombatStrategy>();
  }
  std::fprintf(stderr,
               "[strategy] decision_type=%s 尚未实现，回退占位档 wait\n",
               type.c_str());
  return std::make_unique<WaitStrategy>();
}

}  // namespace rmtc_bot
