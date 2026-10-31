cmake_minimum_required(VERSION 3.8)
project(rmtc_bot)

# 使用 C++17（nlohmann/json 需要）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(Threads REQUIRED)  # llm_client 后台线程

add_executable(rmtc_bot
  src/main.cpp
  src/bot_node.cpp
  src/perception.cpp
  src/llm_client.cpp
  src/llm_conclusion.cpp
  src/llm_brain.cpp
  src/combat_llm_guide.cpp
  src/strategy.cpp
  src/collect_strategy.cpp
  src/combat_strategy.cpp
  # 10/28：决策内核与 LLM 顾问层（docs/决策设计.md）。
  # decision_model 已接入 combat_strategy 的 tick 决策路径（信念/饱和度/参数）；
  # llm_advisor 为 B2 事件处理预备（§6 步骤 3 接线后消费）。
  src/decision_model.cpp
  src/llm_advisor.cpp
)

# 感知解析自检（不依赖 ROS，可单独运行）
add_executable(perception_selftest
  src/perception_selftest.cpp
  src/perception.cpp
)

# LLM 客户端自检（内建模拟网关，不依赖 ROS/网络）
add_executable(llm_selftest
  src/llm_selftest.cpp
  src/llm_client.cpp
)
target_link_libraries(llm_selftest Threads::Threads)

# LLM 结论解析/校验自检（M3 管道预备，不依赖 ROS/网络）
add_executable(llm_conclusion_selftest
  src/llm_conclusion_selftest.cpp
  src/llm_conclusion.cpp
)

# LLM 触发/提示词/结论持有自检（M3 管道，不依赖 ROS/网络）
add_executable(llm_brain_selftest
  src/llm_brain_selftest.cpp
  src/llm_brain.cpp
  src/llm_client.cpp
)
target_link_libraries(llm_brain_selftest Threads::Threads)

# LLM 网关调用演示已删除（10/28 批次四 4.2 审计噪音清理：demo 被 llm_selftest 取代）

# 策略接口与配置加载自检（不依赖 ROS）
# 预链 M3 支撑源（10/21）：strategy 实现层若引用 LlmResult 解析/意图翻译/客户端，
# 链接不炸（/tmp QA 踩过的坑已拆雷）；未引用时这些对象文件不产生任何副作用
add_executable(strategy_selftest
  src/strategy_selftest.cpp
  src/strategy.cpp
  src/collect_strategy.cpp
  src/combat_strategy.cpp
  src/decision_model.cpp
  src/llm_conclusion.cpp
  src/combat_llm_guide.cpp
  src/llm_client.cpp
)
target_link_libraries(strategy_selftest Threads::Threads)

# 第一阶段采集策略离线自检（13 组场景，不依赖 ROS；10/25 策略实现后转绿）
add_executable(collect_selftest
  src/collect_selftest.cpp
  src/collect_strategy.cpp
)

# 第二阶段对抗策略规则层离线自检（13 组场景，不依赖 ROS/LLM；10/27 规则层实现后转绿）
# 同样预链 M3 支撑源：CombatStrategy 接 LLM 意图翻译层/结论解析/客户端时无需改 CMake
# 10/27：组合复用采集内核（B1 方案 b）→ 需链接 collect_strategy.cpp
add_executable(combat_selftest
  src/combat_selftest.cpp
  src/combat_strategy.cpp
  src/collect_strategy.cpp
  src/decision_model.cpp
  src/llm_conclusion.cpp
  src/combat_llm_guide.cpp
  src/llm_client.cpp
)
target_link_libraries(combat_selftest Threads::Threads)

# 决策内核 + LLM 顾问层离线自检（不依赖 ROS/裁判/网络；10/28，docs/决策设计.md）
add_executable(decision_selftest
  src/decision_selftest.cpp
  src/decision_model.cpp
  src/llm_advisor.cpp
)

# LLM 意图翻译层自检（M3：结论→任务排序/回避修正，纯函数不依赖 ROS/网络）
add_executable(combat_llm_guide_selftest
  src/combat_llm_guide_selftest.cpp
  src/combat_llm_guide.cpp
  src/llm_conclusion.cpp
)

# 单头文件 JSON 库（随包携带，无需系统安装）
target_include_directories(rmtc_bot PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(perception_selftest PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(llm_selftest PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(llm_conclusion_selftest PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(llm_brain_selftest PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(combat_llm_guide_selftest PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(strategy_selftest PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(collect_selftest PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(combat_selftest PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(decision_selftest PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

ament_target_dependencies(rmtc_bot rclcpp std_msgs)
target_link_libraries(rmtc_bot Threads::Threads)

# S4（10/27）：八套自检注册为 CTest 测试，`colcon test` 可一键跑
# （selftest.sh 仍保留，两者并行可用）
if(BUILD_TESTING)
  add_test(NAME perception_selftest COMMAND perception_selftest)
  add_test(NAME strategy_selftest COMMAND strategy_selftest)
  add_test(NAME llm_selftest COMMAND llm_selftest)
  add_test(NAME llm_conclusion_selftest COMMAND llm_conclusion_selftest)
  add_test(NAME llm_brain_selftest COMMAND llm_brain_selftest)
  add_test(NAME combat_llm_guide_selftest COMMAND combat_llm_guide_selftest)
  add_test(NAME collect_selftest COMMAND collect_selftest)
  add_test(NAME combat_selftest COMMAND combat_selftest)
  add_test(NAME decision_selftest COMMAND decision_selftest)
endif()

install(TARGETS rmtc_bot perception_selftest llm_selftest llm_conclusion_selftest
  llm_brain_selftest combat_llm_guide_selftest
  strategy_selftest collect_selftest combat_selftest decision_selftest
  DESTINATION lib/${PROJECT_NAME}
)

# 决策档位配置：装进 install/share/rmtc_bot/config/（现场只允许改 decision_type）
install(DIRECTORY config
  DESTINATION share/${PROJECT_NAME}
)

ament_package()
