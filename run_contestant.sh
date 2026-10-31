#!/usr/bin/env bash
# RMTC 参赛程序启动脚本
# 用法: ./run_contestant.sh <席位A-D> <队名> [AI网关地址]
#   席位必填（也可用 RMTC_SEAT 环境变量）：席位是参赛身份，拒绝默认值顶替。
# 所有接入参数通过环境变量可覆盖，禁止硬编码正式配置。
# 注意：不能加 -u —— source /opt/ros/humble/setup.bash 会引用未定义变量
set -eo pipefail

SEAT="${1:-${RMTC_SEAT:-}}"
NAME="${2:-BLACKOUT队}"
GATEWAY="${3:-http://127.0.0.1:8080/v1/chat/completions}"

# 席位守门：缺失或非法（非 A-D 大写单字符）直接中止，宁可启动失败也不注册错误席位
if [[ ! "$SEAT" =~ ^[A-D]$ ]]; then
  echo "用法: ./run_contestant.sh <席位A-D> <队名> [AI网关地址]（席位也可用 RMTC_SEAT 环境变量）" >&2
  echo "[中止] 席位必须显式指定且为 A-D 大写单字符，当前='${SEAT:-<空>}'" >&2
  exit 2
fi

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# ROS 环境缺失时给出明确提示（10/27 审查 S6）
if [ ! -f /opt/ros/humble/setup.bash ]; then
  echo "[中止] 未找到 ROS2 Humble 环境 /opt/ros/humble/setup.bash，请先在官方环境安装 ROS2 Humble" >&2
  exit 1
fi
if [ ! -f "$HERE/contestant_ws/install/setup.bash" ]; then
  echo "[中止] 未找到构建产物 $HERE/contestant_ws/install/setup.bash —— 请先构建：" >&2
  echo "  source /opt/ros/humble/setup.bash && cd $HERE/contestant_ws && colcon build --symlink-install" >&2
  exit 1
fi

source /opt/ros/humble/setup.bash
source "$HERE/contestant_ws/install/setup.bash"

export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-27}"
export ROS_LOCALHOST_ONLY="${ROS_LOCALHOST_ONLY:-0}"
export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}"
export RMTC_TEAM_TOKEN="${RMTC_TEAM_TOKEN:-practice-$SEAT}"
export RMTC_AI_GATEWAY="${RMTC_AI_GATEWAY:-$GATEWAY}"
# llm_client 读 RMTC_LLM_URL；与旧变量 RMTC_AI_GATEWAY 保持一致
export RMTC_LLM_URL="${RMTC_LLM_URL:-$RMTC_AI_GATEWAY}"
# 决策档位配置文件（现场只允许改该文件里的 decision_type 字段）。
# 唯一权威 = 源码树 config（提交包内含源码树；install 副本是构建产物，仅核对）。
CFG_SRC="$HERE/contestant_ws/src/rmtc_bot/config/decision_config.json"
CFG_INST="$HERE/contestant_ws/install/share/rmtc_bot/config/decision_config.json"
if [ -f "$CFG_INST" ] && [ -f "$CFG_SRC" ] && ! cmp -s "$CFG_SRC" "$CFG_INST"; then
  echo "[警告] 源码树与 install 的 decision_config.json 不一致（10/27 审查 M12）：" >&2
  echo "  现场修改请只改源码树文件 $CFG_SRC；重新 colcon build 即同步 install 副本" >&2
fi
export RMTC_DECISION_CONFIG="${RMTC_DECISION_CONFIG:-$CFG_SRC}"

exec ros2 run rmtc_bot rmtc_bot "$SEAT" "$NAME"
