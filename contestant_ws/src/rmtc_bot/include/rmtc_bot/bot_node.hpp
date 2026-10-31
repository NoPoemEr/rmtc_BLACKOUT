#pragma once
// 协议层/通信骨架：本节点只负责"合法地收发"。
//
// 职责（与赛事协议一一对应）：
//   1. 订阅本席位感知 /team_X/perception（RELIABLE / VOLATILE / KEEP_LAST(1)）
//   2. 用 perception 模块解析并校验整帧字段；解析失败不崩溃、本帧不发动作
//   3. 按场次丢弃旧/重复 tick；session_id 变化时清空状态并重新注册
//      （曾离开的场次的迟到帧一律丢弃，不会把当前场次回翻）
//   4. 原样回填 tick / session_id，发布动作 /team_X/action
//   5. 向 /team_X/registration 注册队伍显示名（TRANSIENT_LOCAL）
//   6. 打印解析结果与 last_action 回执，供双向通信确认

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>

#include "rmtc_bot/llm_brain.hpp"
#include "rmtc_bot/perception.hpp"
#include "rmtc_bot/strategy.hpp"

namespace rmtc_bot {

class BotNode : public rclcpp::Node {
 public:
  BotNode(const std::string &seat, const std::string &team_name);

 private:
  // 感知回调：每收到一帧感知调用一次
  void on_perception(const std_msgs::msg::String::SharedPtr msg);

  // 依据解析结果打印本帧信息（验证解析正确性的日志素材）
  void log_perception(const Perception &p);

  // 发布一条动作（tick 由感知原样回填；choice 来自策略）
  void publish_action(int tick, const ActionChoice &choice);

  // 发布/重发队伍注册消息
  void publish_registration();

  // 换场（session_id 变化）时清空上一场状态
  void handle_new_session(const std::string &session_id);

  std::string seat_;        // 本场席位 A/B/C/D
  std::string team_name_;   // 队伍显示名（1–96 UTF-8 字节，无换行）
  std::string session_id_;  // 当前场次 ID（来自感知，原样回填）
  int last_tick_ = -1;      // 最近已处理的 tick（当前场次）
  // M1 修复：按场次记录最后处理到的 tick——旧场迟到帧（tick 不增长）不再回翻
  // 当前场次；曾离开的场次（left_sessions_）的帧一律当迟到帧丢弃。
  std::map<std::string, int> tick_by_session_;
  std::set<std::string> left_sessions_;
  std::unique_ptr<Strategy> strategy_;  // 决策策略（按 decision_config.json 创建）
  std::unique_ptr<LlmBrain> brain_;  // LLM 触发/提示词管道（仅 combat 档创建；collect/wait 档空指针零调用，10/29 拍板）
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr perception_sub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr action_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr registration_pub_;
};

}  // namespace rmtc_bot
