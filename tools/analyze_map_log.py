#!/usr/bin/env python3
# analyze_map_log.py —— 对抗地图实测日志分析器
#
# 用法: python3 tools/analyze_map_log.py <bot日志> [地图config.json]
# 输出: 1) 关键指标（任务/资源/伤害/死亡/动作分布/卡墙/无效动作）
#       2) 位置热力图 + 地图叠加（障碍/资源/任务点/出生点）
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

LOG = Path(sys.argv[1])
CFG = sys.argv[2] if len(sys.argv) > 2 else None

tick_re = re.compile(r"tick=(\d+) 地图=(\d+)x(\d+) 半径=(\d+) pos=\[(\d+),(\d+)\] hp=(\d+) 存活 实体=(\d+) 任务=(\d+)")
act_re = re.compile(r"本Tick动作: (\w+) ?(\w*)（([^）]*)")
rcpt_re = re.compile(r"上Tick回执: tick=(\d+) action=(\w+) moved=(\w+)(?: dir=(\w+))?")
score_re = re.compile(r"得分 资源=([\d.]+) 伤害=([\d.]+) 任务完成=(\d+) 非法动作=(\d+)")

w = h = 0
positions = Counter()
actions = Counter()
reasons = Counter()
blocked_moves = 0
total_moves = 0
deaths = []
score_final = None
task3_tick = None
task_push_after_done = 0
prev_blocked_dir = Counter()
last_tick = 0

lines = LOG.read_text().splitlines()
for i, line in enumerate(lines):
    m = tick_re.search(line)
    if m:
        last_tick = int(m.group(1))
        tick, w, h, r, x, y, hp = (last_tick, int(m.group(2)), int(m.group(3)),
                                   int(m.group(4)), int(m.group(5)), int(m.group(6)),
                                   int(m.group(7)))
        positions[(x, y)] += 1
        continue
    if "阵亡：等待复活" in line:
        deaths.append(last_tick)
        continue
    m = act_re.search(line)
    if m:
        action = m.group(1)
        actions[action] += 1
        reason = m.group(3)
        reasons[reason] += 1
        if "朝任务点" in reason and task3_tick is not None and last_tick >= task3_tick:
            task_push_after_done += 1
        continue
    m = rcpt_re.search(line)
    if m and m.group(2) == "move":
        total_moves += 1
        if m.group(3) == "false":
            blocked_moves += 1
            prev_blocked_dir[m.group(4) or "?"] += 1
        continue
    m = score_re.search(line)
    if m:
        score_final = (m.group(1), m.group(2), m.group(3), m.group(4))
        if task3_tick is None and m.group(3) == "3":
            task3_tick = last_tick

print(f"=== {LOG.name} ===")
print(f"地图 {w}x{h}  任务完成=3 首帧: tick {task3_tick}")
if score_final:
    print(f"终局: 资源={score_final[0]} 伤害={score_final[1]} 任务={score_final[2]} "
          f"非法动作={score_final[3]}")
print(f"任务完成后仍朝任务点推进: {task_push_after_done} 次")
print(f"动作分布: {dict(actions.most_common())}")
print(f"死亡帧: {len(deaths)} 次 (ticks {deaths[:12]}{'...' if len(deaths)>12 else ''})")
print(f"移动被挡: {blocked_moves}/{total_moves}  被挡方向: {dict(prev_blocked_dir.most_common())}")
print(f"原因 TOP12:")
for reason, n in reasons.most_common(12):
    print(f"  {n:4d}  {reason[:80]}")

if CFG and w:
    cfg = json.loads(Path(CFG).read_text())
    obs = {tuple(p) for p in cfg["map"]["obstacles"]}
    res = {tuple(p["position"]): p["value"] for p in cfg["map"]["resources"]}
    spawns = {tuple(r["spawn"]): r["team"] for r in cfg["robots"]}
    reach = next(tuple(m["position"]) for m in cfg["missions"] if m["type"] == "reach")
    print(f"\n位置热力图（数字=停留帧数，0-9/A=10-15... 超过用字母）:")
    for y in range(h):
        row = []
        for x in range(w):
            p = (x, y)
            if p in obs:
                row.append("██")
            elif p == reach:
                row.append("TT")
            elif p in spawns:
                row.append(spawns[p] * 2)
            elif p in res:
                row.append("rR" if res[p] >= 20 else "rr")
            else:
                n = positions.get(p, 0)
                c = str(n) if n < 10 else chr(ord("a") + min(n - 10, 25))
                row.append("·" if n == 0 else c + c)
        print("".join(row))
    top = positions.most_common(5)
    print(f"高频停留格 TOP5: {top}")
    # 距离任务点/中心统计
    cx, cy = w // 2, h // 2
    d_center = sum(positions[(x, y)] * (abs(x - cx) + abs(y - cy))
                   for (x, y) in positions)
    n = sum(positions.values())
    print(f"平均到中心距离: {d_center / max(1, n):.2f} 格（帧加权）")
