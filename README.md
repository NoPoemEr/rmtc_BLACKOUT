# RMTC 机甲大师图灵对决赛 · 参赛程序（BLACKOUT 队）

> 代码仓库：<https://github.com/NoPoemEr/rmtc_BLACKOUT>
> 运行环境：Ubuntu 22.04 + ROS 2 Humble + C++17（colcon 工作空间）
> 技术报告：语雀协作文档，随参赛材料单独提交；本仓库只保留可运行源码、构建脚本与自检工具。

ROS 2 Humble 单节点程序：订阅本席位 `/team_X/perception`，每 Tick 解析感知并决策，向 `/team_X/action` 发布动作；同时向 `/team_X/registration` 发布队名注册信息。

## 目录结构

```text
rmtc_blackout/
├── README.md                    # 本文：环境、构建、启动、阶段切换、模块说明
├── run_contestant.sh            # tools/ 批量验收脚本内部复用；比赛启动不使用（见下）
├── contestant_ws/               # colcon 工作空间
│   └── src/rmtc_bot/            # 参赛包（自包含，可直接构建）
│       ├── CMakeLists.txt
│       ├── package.xml
│       ├── include/rmtc_bot/    # 节点、感知、策略、LLM 管道头文件
│       ├── include/nlohmann/json.hpp  # 单头 JSON 库（随包携带）
│       ├── src/                 # 节点、策略、LLM 客户端及九套自检
│       └── config/decision_config.json  # 只含 decision_type 一个字段
├── tools/                       # 自检、验收入口、提交打包脚本
└── maps/                        # 压力测试地图（复现实验用，不参与运行时决策）
```

`dist/` 仅在执行 `tools/package.sh` 后生成（提交 tarball + sha256），已被 `.gitignore` 忽略。

## 环境与依赖

- Ubuntu 22.04，ROS 2 Humble（桌面版）
- C++17 编译器、CMake、colcon
- ROS 依赖：`rclcpp`、`std_msgs`；系统线程库
- JSON 库：`nlohmann/json.hpp` 已随包携带，不需要额外安装
- 网络：只需能访问主办方提供的 AI 网关；不访问网页、观赛接口或其他席位话题

## 构建

**所有命令都从仓库根目录 `rmtc_blackout/` 或其 `contestant_ws/` 子目录执行**，避免误用其他工作空间的同名程序。

```bash
cd /path/to/rmtc_blackout
source /opt/ros/humble/setup.bash
cd contestant_ws
colcon build
cd ..
source contestant_ws/install/setup.bash
```

构建完成后必须手工 `source contestant_ws/install/setup.bash`；每开一个新终端，都要重新 source ROS 和本工作空间（或把这两行写进该终端的启动配置）。

## 启动（纯命令行）

以下命令不经过 `run_contestant.sh`，直接从仓库根目录用 `ros2 run` 启动。第一阶段（A 席位，采集档）完整命令：

```bash
cd /path/to/rmtc_blackout
source /opt/ros/humble/setup.bash
source contestant_ws/install/setup.bash

export ROS_DOMAIN_ID=27
export ROS_LOCALHOST_ONLY=0
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export RMTC_TEAM_TOKEN='<本场令牌>'
export RMTC_LLM_URL='http://<AI网关地址>:<端口>/v1/chat/completions'
export RMTC_DECISION_CONFIG="$PWD/contestant_ws/src/rmtc_bot/config/decision_config.json"

ros2 run rmtc_bot rmtc_bot A 'BLACKOUT队'
```

第二阶段（裁判分配的席位，对抗档；示例为 B 席位）：把 `decision_type` 改为 `combat`、席位改为裁判分配的 B，其余环境变量不变，重新执行最后一条命令：

```bash
ros2 run rmtc_bot rmtc_bot B 'BLACKOUT队'
```

席位是参赛身份，直接作为 `ros2 run` 后的第 1 个参数传入，决定注册的 ROS 话题与动作归属；缺失或不是 A–D 大写单字符时，节点会直接报错退出，避免注册错席位。`RMTC_DECISION_CONFIG` 必须使用上面这样的**绝对路径**（或从仓库根目录传入源码路径）；否则当前目录不对时，配置会回退到 `wait`。

## 接入参数

全部接入参数通过环境变量传入，不硬编码正式配置：

| 变量 | 示例值 / 代码默认 | 说明 |
|---|---|---|
| `ROS_DOMAIN_ID` | 示例 27 | 正式赛按主办方公布值设置 |
| `ROS_LOCALHOST_ONLY` | 示例 0 | 0 = 允许跨机通信 |
| `RMW_IMPLEMENTATION` | 示例 `rmw_fastrtps_cpp` | 与裁判端保持一致 |
| `RMTC_TEAM_TOKEN` | 无默认，必须设置 | 本场队伍令牌；未设置时 LLM 客户端禁用，第二阶段会丢 AI 资格 |
| `RMTC_LLM_URL` | 无默认，必须设置 | AI 网关完整 URL；未设置时 LLM 客户端禁用 |
| `RMTC_LLM_MODEL` | `deepseek-flash` | 请求模型名；设为空字符串表示不发送 `model` 字段 |
| `RMTC_LLM_THINKING` | `disabled` | `disabled`/`enabled` 发送 thinking 字段；`off`/`none`/空 = 不发送 |
| `RMTC_LLM_MAX_TOKENS` / `RMTC_LLM_TIMEOUT_MS` | 512 / 6000 | LLM 输出上限 / 连接读超时（毫秒） |
| `RMTC_LLM_TEMPERATURE` / `RMTC_LLM_RESPONSE_FORMAT` | 0.2 / `json_object` | 结论 JSON 稳定性；`none` 关闭 response_format |
| `RMTC_DECISION_CONFIG` | 代码默认相对 `config/decision_config.json`；纯命令行必须显式设绝对路径 | 决策档位配置文件；当前目录不对时会回退 `wait` |

> 令牌只通过环境变量传入，禁止写入源码、README、截图或提交包。材料中不得包含模型密钥或正式令牌。

> `run_contestant.sh` 仅保留给 `tools/` 下的批量验收脚本复用，不作为比赛启动方式；比赛启动以上面的 `ros2 run` 命令为准。

## 第一 / 第二阶段如何切换

仓库中的 `contestant_ws/src/rmtc_bot/config/decision_config.json` 只允许包含 `decision_type` 一个字段：

```json
{
  "decision_type": "collect"
}
```

| 阶段 | `decision_type` | 启动席位 | 说明 |
|---|---|---|---|
| 第一阶段 | `collect` | A | 采集策略；本档不调用 LLM |
| 第二阶段 | `combat` | 裁判分配 A–D | 对抗策略；启用 LLM 任务/事件理解 |
| 联调排障 | `wait` | 任意 | 每 Tick 合法 WAIT，用于验证通信链路 |

切换步骤（按规则在暂停期间或加赛前提出，经裁判登记后执行）：

1. 停止当前程序（Ctrl-C）。
2. 只修改 `decision_config.json` 的 `decision_type` 值，例如 `collect` → `combat`；不要改其他字段。
3. 从仓库根目录重新执行 `ros2 run rmtc_bot rmtc_bot <席位> 'BLACKOUT队'`；启动前确认 `RMTC_DECISION_CONFIG` 仍指向源码树中的该文件。

`RMTC_DECISION_CONFIG` 必须指向**源码树**中的该文件（见上文完整命令），因此切换档位**不需要重新 `colcon build`**；程序启动时读取一次，修改后必须重启进程才生效。若重新构建，`install/share/rmtc_bot/config/` 下的副本会同步，但运行时仍以 `RMTC_DECISION_CONFIG` 指定的文件为准。

比赛期间只允许改这一个字段，禁止现场修改源代码、算法实现、Prompt、其他决策参数或人工发送具体动作。每场最多 2 次人工策略切换。

## 主要模块与运行逻辑

数据流：

```text
/team_X/perception (std_msgs/String, JSON)
        │
        ▼
perception.cpp  解析 tick / session_id / self / 资源 / 车辆 / 任务 / 事件
        │
        ▼
Strategy（由 decision_config.json 选择）
  ├─ CollectStrategy   第一阶段：记忆地图 + BFS + 前沿探索 + 资源目标
  └─ CombatStrategy    第二阶段：规则漏斗 + 任务/资源规划 + 攻击/防御/撤离
        │             └─ LlmBrain（仅 combat）：异步调用官方 AI 网关
        ▼
/team_X/action (std_msgs/String, JSON，每 Tick 至多一条)
```

| 模块 | 职责 |
|---|---|
| `bot_node.cpp` | ROS 节点：订阅本席位感知、维护 session/tick、调用策略、发布动作与队名注册；丢弃旧 Tick/旧场次迟到帧 |
| `perception.cpp` | 解析官方感知 JSON，归一化实体类型/方向/浮点伤害，提取任务与自然语言事件 |
| `strategy.cpp` | 按 `decision_type` 创建 `Wait` / `Collect` / `Combat` 策略；配置缺失或损坏时回退 `wait`，不崩溃 |
| `collect_strategy.cpp` | 第一阶段采集：已见格记忆、每帧 BFS、最近资源优先、无资源时前沿探索 |
| `combat_strategy.cpp` | 第二阶段对抗：生存/任务/资源/追击/攻击/防御/撤离的规则决策漏斗 |
| `decision_model.cpp` | 候选目标生成、EV 打分、信念与饱和度等决策内核 |
| `llm_client.cpp` | 唯一 AI 网关出口：POSIX socket HTTP POST，超时、错误码与配额降级 |
| `llm_brain.cpp` | combat 档的 LLM 触发、提示词组装、结论持有与时效管理 |
| `llm_conclusion.cpp` | LLM 结论 JSON 的本地校验、合法性与时效检查，失败回退规则层 |
| `combat_llm_guide.cpp` | 把 LLM 结构化结论翻译成任务排序/回避修正，不解析事件原文关键词 |

合规边界：程序只订阅本席位 `/team_X/perception`，只发布本席位 `/team_X/action` 与 `/team_X/registration`；不读取观赛画面、裁判管理数据、测试配置或其他席位感知。自然语言事件整段交给官方 AI 网关理解，代码不做关键词/正则/固定句式匹配。

## 自检与提交打包

九套离线自检（不依赖裁判；建议每次改动后全跑）：

```bash
./tools/selftest.sh --build     # 全量重建 + 九套自检 + 汇总
./tools/selftest.sh             # 不重建，直接跑九套
./tools/package.sh              # git 干净导出 → 干净构建 → 九套自检 → dist/ tarball + sha256
```

单独运行：

```bash
source /opt/ros/humble/setup.bash
source contestant_ws/install/setup.bash
ros2 run rmtc_bot perception_selftest
ros2 run rmtc_bot strategy_selftest
ros2 run rmtc_bot collect_selftest
ros2 run rmtc_bot combat_selftest
ros2 run rmtc_bot llm_selftest
ros2 run rmtc_bot llm_conclusion_selftest
ros2 run rmtc_bot llm_brain_selftest
ros2 run rmtc_bot combat_llm_guide_selftest
ros2 run rmtc_bot decision_selftest
```

`tools/package.sh` 带有脏工作区守门：有未提交改动会中止，避免提交包落后于源码。生成的 `dist/rmtc_bot_submission_*.tar.gz` 只含源码、配置、工具和地图，不含 `build/`、`install/`、`log/`、`docs/` 或令牌。

## 验证记录（简要）

- 10/20：测试端 stage1/stage2 双向通信、逐帧回执闭环、`invalid_actions=0`；暂停恢复、重开、`kill -9` 后重启均恢复。
- 10/25：第一阶段采集策略 stage1 三场 40/40 满分，`invalid=0`，回执闭环。
- 10/28：第二阶段真实 LLM 整场 27/27 调用成功、零超时、`invalid=0`，49.8 分。
- 10/31：V8.7 在 40×40 全资源图 500 Tick 复核 **45.0 分第 1、资源 120/120、任务 3/3、`invalid=0`、零阵亡**；同场 0 伤害，战斗线仍需在混战图单独验证。
- 多墙、迷宫、长廊等压力图的结果与复现方式见技术报告；源码自检九套全绿。

## 提交说明

- 代码仓库：<https://github.com/NoPoemEr/rmtc_BLACKOUT>（工作人员可直接访问）
- 技术报告：语雀协作文档（含设计思路、功能完成情况、分工、日程、问题解决、AI 协同、测试截图与运行指令），按通知提交链接并将群主/学辅设为协作者。
- 代码出包：`./tools/package.sh`；当前提交版本以 `git log -1` 为准。比赛启动目录固定为 `<仓库根>`，启动命令为 `ros2 run rmtc_bot rmtc_bot <席位> 'BLACKOUT队'`，接入参数按上表通过 `export` 注入。
- 前置检录截图/录屏放在技术报告中；仓库与提交包不包含模型密钥、正式令牌或裁判管理数据。
