#!/usr/bin/env bash
# 存档裁判 /api/state 快照，供检录材料对号（默认 docs/evidence/，已 git 忽略）
# 用法: ./tools/snapshot.sh <名字> [端口]
#   例: ./tools/snapshot.sh stage1_m2        → docs/evidence/stage1_m2_20261025-101500.json
#   RMTC_EVIDENCE_DIR=/path/to/evidence ./tools/snapshot.sh stage1_m2  # 可在仓库外存档
set -eo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/.."
NAME="${1:?用法: ./tools/snapshot.sh <名字> [端口]}"
# L25 修复：名字白名单清洗（字母/数字/._-），防路径穿越与空格
case "$NAME" in
  *[!A-Za-z0-9_.-]*|.|..)
    echo "[中止] 名字只允许字母/数字/._-，当前='$NAME'" >&2
    exit 2
    ;;
esac
PORT="${2:-8080}"
STAMP="$(date +%Y%m%d-%H%M%S)"
EVI="${RMTC_EVIDENCE_DIR:-$ROOT/docs/evidence}"
mkdir -p "$EVI"
DEST="$EVI/${NAME}_${STAMP}.json"

STATE="$(curl -sf "http://127.0.0.1:$PORT/api/state")"
echo "$STATE" > "$DEST"
echo "已存: $DEST"
echo "run_dir: $(echo "$STATE" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("run_dir",""))' 2>/dev/null || echo '?')"
echo "提示: 通信日志在 run_dir 下 communication.jsonl / ai.jsonl / score.json"
