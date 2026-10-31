#!/usr/bin/env bash
# 九套离线自检一键跑（不依赖裁判/ROS 网络，纯本地）
# 用法: ./tools/selftest.sh [--build]
#   --build  先全量重新构建（默认跳过，直接用现有 build）
# 注意：不能加 -u —— source /opt/ros/humble/setup.bash 会引用未定义变量
set -eo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WS="$HERE/../contestant_ws"

if [ "${1:-}" = "--build" ]; then
  echo ">>> 重新构建（colcon build --symlink-install）"
  source /opt/ros/humble/setup.bash
  (cd "$WS" && colcon build --symlink-install)
fi

source /opt/ros/humble/setup.bash
source "$WS/install/setup.bash"

# 名称 / 预期结果说明（只做事实呈现，不替实现状态下结论）
#   collect：10/25 已实现 → 应绿
#   combat ：10/27 已实现 → 应绿
TESTS=(
  "perception_selftest|perception 感知解析|任何时刻都应绿"
  "strategy_selftest|strategy 策略接口/配置|任何时刻都应绿"
  "llm_selftest|llm LLM 客户端|任何时刻都应绿"
  "llm_conclusion_selftest|llm_conclusion LLM 结论解析/校验|任何时刻都应绿"
  "llm_brain_selftest|llm_brain LLM 触发/提示词|任何时刻都应绿"
  "combat_llm_guide_selftest|combat_llm_guide LLM 意图翻译层|任何时刻都应绿"
  "collect_selftest|collect 第一阶段策略|10/25 实现后应绿"
  "combat_selftest|combat 第二阶段规则层|10/27 实现后应绿"
  "decision_selftest|decision 决策内核+LLM顾问|10/28 落地后应绿"
)

printf '%-22s %-6s %s\n' "自检" "结果" "说明"
printf '%s\n' "------------------------------------------------------------"
ALLGREEN=1
for entry in "${TESTS[@]}"; do
  bin="${entry%%|*}"
  rest="${entry#*|}"
  label="${rest%%|*}"
  note="${rest#*|}"
  set +e  # 红灯（退出码 1）是正常业务结果，不能触发 errexit
  out="$(ros2 run rmtc_bot "$bin" 2>&1)"; rc=$?
  set -e
  if [ $rc -eq 0 ]; then
    printf '%-22s %-6s %s\n' "$label" "绿" "$note"
  else
    ALLGREEN=0
    printf '%-22s %-6s %s\n' "$label" "红" "$note"
    # M7 修复：grep 无匹配会退出 1，在 set -e + pipefail 下会把整个脚本
    # 当场中断；先取回匹配文本（|| true），再 head/sed 展示。
    fail_lines="$(printf '%s\n' "$out" | grep -E '\[FAIL\]' || true)"
    if [ -n "$fail_lines" ]; then
      printf '%s\n' "$fail_lines" | head -4 | sed 's/^/    /'
    fi
  fi
done

echo
if [ $ALLGREEN -eq 1 ]; then
  echo ">>> 全部自检通过"
  exit 0
else
  echo ">>> 有红灯：九套自检任一红灯都需排查（M8 修复：红灯时脚本退出码为 1，可机检）"
  exit 1
fi
