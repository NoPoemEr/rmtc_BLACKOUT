#!/usr/bin/env python3
"""suite_digest.py —— 全流程测试单场速览（给报告评述用）

用法: python3 tools/suite_digest.py <final_state.json> [bot.log]
读 run_dir/frames.jsonl，输出：
  - 终局四队表
  - 我方 A 的资源/伤害里程碑、任务完成 tick、阵亡 tick
  - 每台敌车与我方 A 的"近身接触"统计（曼哈顿 ≤2 的 tick 数、首次接触 tick、累计/最长连续接触）
  - 每台敌车的资源/伤害/阵亡
"""
import json
import sys
from pathlib import Path


def load_frames(run_dir):
    rows = []
    for line in open(Path(run_dir) / "frames.jsonl", errors="ignore"):
        d = json.loads(line)
        mv = d["map_view"]
        if isinstance(mv, str):
            mv = eval(mv)  # noqa: S307
        rows.append((d["tick"], {t["team"]: t for t in d["teams"]},
                     {r["team"]: r for r in mv["robots"]}))
    return rows


def main():
    snap = json.load(open(sys.argv[1]))
    rows = load_frames(snap["run_dir"])
    teams = sorted(snap["board"]["teams"], key=lambda t: -t.get("score", 0))
    print("== 终局 ==")
    for i, t in enumerate(teams, 1):
        print(f"  #{i} {t['team']} {t.get('team_name','')} 分={t.get('score')} "
              f"资源={t.get('resource_value')} 伤害={t.get('effective_damage')} "
              f"任务={t.get('mission_completed')} invalid={t.get('invalid_actions')} "
              f"hp={t.get('hp')} alive={t.get('alive')}")

    def milestones(team):
        res, dmg = [], []
        mission_ticks, deaths = [], []
        was_alive = True
        last_mis = 0
        for tk, tb, rb in rows:
            t, r = tb.get(team), rb.get(team)
            if not t or not r:
                continue
            res.append((tk, t.get("resource_value", 0)))
            dmg.append((tk, t.get("effective_damage", 0)))
            mis = t.get("mission_completed", 0)
            if mis > last_mis:
                mission_ticks.append(tk)
                last_mis = mis
            alive = bool(r.get("alive", True))
            if was_alive and not alive:
                deaths.append(tk)
            was_alive = alive

        def first(seq, th):
            for tk, v in seq:
                if v >= th:
                    return tk
            return None

        res_txt = "/".join(str(first(res, th) or "—") for th in (30, 60, 90, 120))
        dmg_txt = "/".join(str(first(dmg, th) or "—") for th in (100, 300, 500))
        print(f"  资源30/60/90/120@ {res_txt}  伤害100/300/500@ {dmg_txt}  "
              f"任务@ {'/'.join(map(str, mission_ticks)) if mission_ticks else '—'}  "
              f"阵亡@ {'/'.join(map(str, deaths)) if deaths else '无'}")

    print("== 我方 A ==")
    milestones("A")
    print("== 敌车 ==")
    for t in ("B", "C", "D"):
        milestones(t)

    print("== 与 A 的近身接触（曼哈顿≤2）==")
    apos = {}
    for tk, tb, rb in rows:
        if "A" in rb:
            apos[tk] = tuple(rb["A"]["position"])
    for t in ("B", "C", "D"):
        near, run, best, total = [], 0, 0, 0
        for tk, tb, rb in rows:
            r, a = rb.get(t), apos.get(tk)
            if not r or not a or not r.get("alive", True) or not rb.get("A", {}).get("alive", True):
                run = 0
                continue
            d = abs(r["position"][0] - a[0]) + abs(r["position"][1] - a[1])
            if d <= 2:
                near.append(tk)
                run += 1
                best = max(best, run)
                total += 1
            else:
                run = 0
        print(f"  {t}: 近身 tick {total}（首次 {near[0] if near else '—'}，最长连续 {best}）"
              f"{'，近身 tick 列表 ' + ','.join(map(str, near[:12])) + ('…' if len(near) > 12 else '') if near else ''}")


if __name__ == "__main__":
    main()
