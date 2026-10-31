#!/usr/bin/env python3
"""analyze_stress.py —— 从 frames.jsonl 还原单场资源/任务/伤害时间线

用法:
  python3 tools/analyze_stress.py <session_dir> [--step 25]
"""
import argparse
import json
import sys
from pathlib import Path


def load(path):
    out = []
    for line in open(path, encoding="utf-8"):
        d = json.loads(line)
        mv = d["map_view"]
        if isinstance(mv, str):
            mv = eval(mv)  # noqa: S307
        teams = {t["team"]: t for t in d["teams"]}
        robots = {r["team"]: r for r in mv["robots"]}
        out.append((d["tick"], teams, robots))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("session")
    ap.add_argument("--step", type=int, default=25)
    args = ap.parse_args()
    frames = load(Path(args.session) / "frames.jsonl")
    if not frames:
        print("no frames")
        return
    teams = "ABCD"
    print("tick " + " ".join(f"{t}:res/dmg/mis" for t in teams))
    for tick, ts, _ in frames:
        if tick % args.step and tick != frames[-1][0]:
            continue
        cells = []
        for t in teams:
            x = ts.get(t, {})
            cells.append(f"{x.get('resource_value',0)}/{x.get('effective_damage',0)}/{x.get('mission_completed',0)}")
        print(f"{tick:4d} " + " ".join(cells))
    # 任务完成 tick
    print("\nmission ticks:")
    for t in teams:
        prev = 0
        hits = []
        for tick, ts, _ in frames:
            n = ts.get(t, {}).get("mission_completed", 0)
            if n != prev:
                hits.append(f"{tick}:{prev}->{n}")
                prev = n
        print(f"  {t}: {' '.join(hits) if hits else '-'}")
    # A 资源/伤害增量事件（按 10 点/20 点粗粒度）
    print("\nA resource steps:")
    prev = 0
    for tick, ts, _ in frames:
        n = ts["A"].get("resource_value", 0)
        if n != prev:
            print(f"  t{tick}: {prev}->{n}")
            prev = n
    print("\nA damage milestones:")
    for target in (100, 200, 300, 400, 500, 750, 1000, 1500):
        hit = next((tick for tick, ts, _ in frames if ts["A"].get("effective_damage", 0) >= target), None)
        print(f"  D>={target}: {hit}")


if __name__ == "__main__":
    main()
