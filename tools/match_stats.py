#!/usr/bin/env python3
"""练习赛单场关键指标（确定性，不调 LLM）。

用法:
  python3 tools/match_stats.py <session_dir> [bot_log]

session_dir 形如 ~/.rmtc-practice/practice-xxxx（含 frames.jsonl）。
bot_log 可选：给了才能算"被命中 vs 同 tick 开火"与决策分布。

输出（全部可从 frames.jsonl / bot 日志复算）：
  1) 终局排名 + 分数构成（资源 30×min(R/120,1)、伤害 15×min(D/500,1)、任务 5/个）
  2) 我们的击杀 tick / 阵亡 tick / 饱和 tick（调整伤害首次 ≥500）
  3) 被命中 tick 数、其中同 tick 也开火的次数、"敌在射程内却没开火"的 tick
  4) 决策分布（按 reason 前缀）
"""
import collections
import json
import re
import sys


def load_frames(path):
    frames = {}
    for line in open(path, encoding="utf-8"):
        d = json.loads(line)
        mv = d["map_view"]
        if isinstance(mv, str):
            mv = eval(mv)  # noqa: S307 —— 练习器输出的 python repr
        robots = {r["team"]: (tuple(r["position"]), r["hp"], r["alive"])
                  for r in mv["robots"]}
        teams = {x["team"]: x for x in d["teams"]}
        frames[d["tick"]] = (robots, teams)
    return frames


def load_actions(log_path):
    acts, cur = {}, None
    for ln in open(log_path, encoding="utf-8", errors="ignore"):
        m = re.search(r"tick=(\d+) .*?hp=(\d+)", ln)
        if m:
            cur = int(m.group(1))
            acts.setdefault(cur, {})["hp"] = int(m.group(2))
        m2 = re.search(r"本Tick动作: (.+?)(?:（(.*)）)?$", ln.rstrip("\n"))
        if m2 and cur is not None:
            acts[cur]["act"] = m2.group(1).strip()
            acts[cur]["why"] = m2.group(2) or ""
    return acts


def straight(a, b):
    if a[0] == b[0]:
        return abs(a[1] - b[1])
    if a[1] == b[1]:
        return abs(a[0] - b[0])
    return None


def main():
    sess = sys.argv[1].rstrip("/")
    logp = sys.argv[2] if len(sys.argv) > 2 else None
    frames = load_frames(sess + "/frames.jsonl")
    ticks = sorted(frames)
    last = frames[ticks[-1]][1]

    print("== 终局 ==")
    for i, t in enumerate(sorted(last.values(), key=lambda x: -x.get("score", -1)), 1):
        res = min(t.get("resource_value", 0) / 120.0, 1.0) * 30
        dmg = min(t.get("effective_damage", 0) / 500.0, 1.0) * 15
        mis = t.get("mission_completed", 0) * 5
        print(f"  #{i} {t['team']}: {t.get('score')} "
              f"(资源 {t.get('resource_value')}→{res:.1f} 伤害 {t.get('effective_damage')}→{dmg:.1f} "
              f"任务 {t.get('mission_completed')}→{mis:.0f} invalid={t.get('invalid_actions')} "
              f"hp={t.get('hp')} alive={t.get('alive')})")

    sat = None
    kills, deaths = [], []
    for i in range(1, len(ticks)):
        tk, (rob, teams) = ticks[i], frames[ticks[i]]
        ptk, (prob, pb) = ticks[i - 1], frames[ticks[i - 1]]
        if sat is None and teams["A"]["effective_damage"] >= 500:
            sat = tk
        if prob["A"][1] > 0 and rob["A"][1] == 0:
            deaths.append(tk)
        for t in ("B", "C", "D"):
            if prob[t][1] > 0 and rob[t][1] == 0:
                dd = {k: teams[k]["effective_damage"] - pb[k]["effective_damage"]
                      for k in "ABCD"}
                dd.pop(t, None)
                if dd and max(dd, key=lambda k: dd[k]) == "A":
                    kills.append((tk, t))
    print(f"\n== 战斗 ==\n  饱和 tick(调整伤害≥500)={sat} "
          f"击杀 {len(kills)} 阵亡 {len(deaths)}")
    print("  击杀:", " ".join(f"{tk}{t}" for tk, t in kills))
    print("  阵亡:", " ".join(str(t) for t in deaths))

    if logp:
        acts = load_actions(logp)
        hits = traded = 0
        bad = []
        # 帧对齐：frames[N] = tick N 的感知（= tick N-1 结算后）。
        # 掉血发生在 ticks[N-1]→ticks[N] 之间，用的是 tick N-1 的动作，
        # 攻击按移动前坐标结算（= frames[N-1] 的位置）。
        for i in range(1, len(ticks)):
            tk, (rob, _) = ticks[i], frames[ticks[i]]
            ptk, (prob, _) = ticks[i - 1], frames[ticks[i - 1]]
            if rob["A"][1] >= prob["A"][1] or ptk not in acts:
                continue
            hits += 1
            act = acts[ptk].get("act", "")
            if act.startswith("attack"):
                traded += 1
                continue
            pa = prob["A"][0]
            inr = [(t, straight(pa, prob[t][0])) for t in "BCD"
                   if prob[t][1] > 0 and straight(pa, prob[t][0])
                   and straight(pa, prob[t][0]) <= 2]
            bad.append((ptk, prob["A"][1] - rob["A"][1], act, inr))
        print(f"\n== 交火 ==\n  被命中 {hits} tick；同 tick 开火 {traded}；"
              f"没开火 {len(bad)}（其中敌在射程内 {len([b for b in bad if b[3]])}）")
        for tk, dmg, act, inr in bad:
            flag = "可还手" if inr else "      "
            print(f"   {flag} tick {tk} 掉血 {dmg} {act} {inr}")

        dist = collections.Counter()
        for tk, a in acts.items():
            if "act" not in a:
                continue
            key = a["why"].split("：")[0].split("（")[0].strip() or a["act"]
            dist[(a["act"], key)] += 1
        print("\n== 决策分布 ==")
        for (act, why), n in dist.most_common():
            print(f"  {n:4d}  {act:7s} {why}")


if __name__ == "__main__":
    main()
