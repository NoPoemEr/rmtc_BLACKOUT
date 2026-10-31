#include <rclcpp/rclcpp.hpp>

#include <csignal>
#include <string>

#include "rmtc_bot/bot_node.hpp"
// 用法：
//   在终端命令 ros2 run rmtc_bot rmtc_bot <席位A-D> [队名]
// 或
//   运行 run_contestant.sh 自动设置 ROS 环境变量
// 席位决定注册话题 /team_X/* 和动作身份，规则不允许默认值顶替：
// 缺失或非法（非 A-D 大写单字符）直接报错退出，宁可启动失败也不注册错误席位。
// 队名只是显示名（不参与动作路由/鉴权），允许默认 BLACKOUT队。
int main(int argc, char **argv) {
  // 忽略 SIGPIPE（10/27 审查 H2 修复）：LLM 网关断连/RST 的瞬间 socket 写
  // 不再杀死整个 ROS 节点进程（llm_client 的 send 同时带 MSG_NOSIGNAL 双保险）。
  std::signal(SIGPIPE, SIG_IGN);

  rclcpp::init(argc, argv);

  const std::string seat_arg = (argc > 1) ? argv[1] : "";
  if (seat_arg.size() != 1 || seat_arg.find_first_not_of("ABCD") != std::string::npos) {
    RCLCPP_ERROR(rclcpp::get_logger("rmtc_bot_main"),
                 "席位必须显式指定且为 A-D 大写单字符");
    rclcpp::shutdown();
    return 2;
  }

  const std::string seat = seat_arg;
  const std::string team_name = (argc > 2) ? argv[2] : "BLACKOUT队";

  try {
    auto node = std::make_shared<rmtc_bot::BotNode>(seat, team_name);
    rclcpp::spin(node);
  } catch (const std::exception &e) {
    RCLCPP_ERROR(rclcpp::get_logger("rmtc_bot_main"), "启动失败: %s", e.what());
    rclcpp::shutdown();
    return 1;
  }

  rclcpp::shutdown();
  return 0;
}
