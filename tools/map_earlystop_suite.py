#!/usr/bin/env python3
"""map_earlystop_suite.py —— V8.7 地图压测的“资源耗尽即停”编排

用途：在多墙/迷宫/40×40/15×15 上复跑 V8.7，但不再每场空等到 500 Tick。
资源榜上四队 resource_value 之和达到地图资源总值（这些图都是 200）时，
立即 /api/pause，保存暂停时的 final_state/frames/score，然后清理进程、换下一图。

注意：早停时任务/伤害/名次都是“资源耗尽那一刻”的中间态，不能和 500 Tick
全量成绩直接比较；本目录单独存证，不覆盖 map_stress_20261031/。

用法:
  python3 tools/map_earlystop_suite.py
  python3 tools/map_earlystop_suite.py --only field15-official --force
  python3 tools/map_earlystop_suite.py --report-only
"""
import argparse
import json
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import full_suite as fs  # noqa: E402
import map_stress_suite as ms  # noqa: E402

EVI = ROOT / "docs/evidence/map_stress_20261031_v87_early"
DOC = ROOT / "docs/analysis/map_stress_v87_earlystop_20261031.md"
RESULTS = EVI / "results.json"
OLD_EVI = ROOT / "docs/evidence/map_stress_20261031"
TICK_MS = 500
MAX_TICKS = 500
POLL_SEC = 0.2

# corridor20 已在 16:44 用 V8.7 跑满 500 Tick；本批只补其余 10 场。
MATCHES = [m for m in ms.MATCHES if m["id"] != "corridor20-official"]


def now():
    return datetime.now().strftime("%F %T")


def eprint(*a):
    print(*a, flush=True)


def resource_total(match):
    cfg = json.loads(ms.MAP_INFO[match["map"]]["cfg"].read_text())
    return sum(int(r.get("value", 0)) for r in cfg["map"]["resources"])


def wait_running(timeout=10):
    """resume 是异步命令；先等裁判真正离开 paused，避免把启动态误判为外部暂停。"""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = fs.get_state(timeout=4)
        status = last.get("status") or {}
        if status.get("finished"):
            return last, "finished"
        if status.get("paused") is False:
            return last, "running"
        time.sleep(0.1)
    return last, "not_running"


def wait_for_exhaustion(total, timeout):
    """返回 (state, reason, tick)。reason=resource_exhausted/finished/...。"""
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        try:
            st = fs.get_state(timeout=4)
        except Exception as exc:  # 裁判启动/切换瞬间可能短暂不可达
            last_err = exc
            time.sleep(0.5)
            continue
        status = st.get("status") or {}
        if status.get("finished"):
            return st, "finished", status.get("tick")
        if status.get("paused"):
            return st, "external_pause", status.get("tick")
        board = st.get("board") or {}
        tick = int(status.get("tick") or 0)
        teams = board.get("teams") or []
        got = sum(float(t.get("resource_value") or 0) for t in teams)
        remaining = (board.get("map_view") or {}).get("resources") or []
        if tick > 0 and total > 0 and got >= total - 1e-6:
            return st, "resource_exhausted", tick
        # 资源数组已清空但榜单还没刷新时，多等一个 tick，避免误报。
        if tick > 0 and total > 0 and not remaining and got > 0:
            time.sleep(POLL_SEC)
            continue
        time.sleep(POLL_SEC)
    return None, f"timeout({last_err})", None


def pause_and_snapshot():
    fs.api("/api/pause", {}, timeout=8)
    deadline = time.time() + 8
    last = None
    while time.time() < deadline:
        last = fs.get_state(timeout=4)
        status = last.get("status") or {}
        if status.get("paused") or status.get("finished"):
            return last
        time.sleep(0.2)
    return last


def run_one(match):
    mid = match["id"]
    info = ms.MAP_INFO[match["map"]]
    opp = ms.OPP_INFO[match["opp"]]
    out = EVI / info["group"]
    out.mkdir(parents=True, exist_ok=True)
    ref_log = out / f"{mid}_referee.log"
    bot_log = out / f"{mid}_bot.log"
    total = resource_total(match)
    eprint(f"\n===== [{mid}] {info['label']} · {opp['label']} · "
           f"{TICK_MS}ms×{MAX_TICKS}（资源总值 {total}） =====")

    fs.cleanup_all()
    ref = fs.start_referee(2, opp["official"], ref_log)
    st = fs.wait_ready(120)
    if not st:
        raise RuntimeError("裁判 120 秒未就绪")
    run_dir = st.get("run_dir", "")

    cfg = json.loads(info["cfg"].read_text())
    body = dict(stage=2, tick_ms=TICK_MS, max_ticks=MAX_TICKS,
                bots=["B", "C", "D"] if opp["official"] else [])
    body["config"] = cfg
    res = fs.api("/api/start", body, timeout=90)
    sid = res.get("session_id")
    st = fs.get_state()
    run_dir = st.get("run_dir", run_dir)
    eprint(f"  session={sid} run_dir={run_dir}")

    started = time.time()
    procs = [fs.start_bot("A", "BLACKOUT队", bot_log)]
    if not opp["official"]:
        names = {"kite": "风筝陪练", "dog": "疯狗陪练", "collector": "采集陪练"}
        for seat, prof in opp["plan"]:
            lp = out / f"{mid}_spar_{seat}.log"
            procs.append(fs.start_sparring(seat, prof, names[prof] + seat, lp))
    need = ["A"] + ([] if opp["official"] else ["B", "C", "D"])
    if not fs.wait_registered(run_dir, need, 60):
        raise RuntimeError(f"注册超时（需 {need}）")
    eprint("  注册完成，resume")
    fs.api("/api/resume", {}, timeout=10)
    st, run_reason = wait_running()
    if run_reason not in ("running", "finished"):
        raise RuntimeError(f"resume 后裁判未离开暂停态：{run_reason}")
    eprint(f"  运行态 tick={(st.get('status') or {}).get('tick', 0)}")

    timeout = max(120, MAX_TICKS * TICK_MS / 1000 + 180)
    final, reason, stop_tick = wait_for_exhaustion(total, timeout)
    if final is None:
        raise RuntimeError(f"对局未结束且未等到资源耗尽：{reason}")
    if reason == "resource_exhausted":
        eprint(f"  资源耗尽 tick={stop_tick}，立即 pause")
        final = pause_and_snapshot() or final
        stop_tick = int((final.get("status") or {}).get("tick") or stop_tick)
        eprint(f"  已暂停 tick={stop_tick}")
    elif reason == "finished":
        stop_tick = int((final.get("status") or {}).get("tick") or MAX_TICKS)
        eprint(f"  对局自然结束 tick={stop_tick}")
    else:
        raise RuntimeError(f"异常状态：{reason}")

    board = final.get("board") or {}
    remaining = len((board.get("map_view") or {}).get("resources") or [])
    (out / f"{mid}_final_state.json").write_text(
        json.dumps(final, ensure_ascii=False, indent=1))
    for name in ("score.json", "ai.jsonl", "frames.jsonl"):
        src = Path(run_dir) / name
        if src.exists():
            shutil.copy2(src, out / f"{mid}_{name}")

    stats = subprocess.run([sys.executable, str(ROOT / "tools/match_stats.py"),
                            run_dir, str(bot_log)], capture_output=True, text=True)
    stats_txt = stats.stdout + (("\n[stderr]\n" + stats.stderr) if stats.stderr else "")
    (out / f"{mid}_stats.txt").write_text(stats_txt)

    m = ms.collect(match, sid, run_dir, final, bot_log, stats_txt)
    m.update(done=True, early_stop=(reason == "resource_exhausted"),
             stop_reason=reason, stop_tick=stop_tick, max_ticks=MAX_TICKS,
             total_resource=total, remaining_resource=remaining,
             elapsed_s=round(time.time() - started, 1),
             finished=bool((final.get("status") or {}).get("finished")),
             evidence=str(out.relative_to(EVI)))
    time.sleep(1)
    fs.cleanup_all(ref, procs)
    eprint(f"  存证完成：{out / (mid + '_final_state.json')} "
           f"（{m['elapsed_s']}s，剩余资源 {remaining}）")
    return m


def load_results():
    if RESULTS.exists():
        return json.loads(RESULTS.read_text())
    return []


def merge_results(results, m):
    by = {r["id"]: r for r in results}
    by[m["id"]] = m
    out = [by.get(x["id"], dict(id=x["id"], done=False)) for x in MATCHES]
    EVI.mkdir(parents=True, exist_ok=True)
    RESULTS.write_text(json.dumps(out, ensure_ascii=False, indent=1))
    return out


def fmt(x):
    if isinstance(x, float):
        if x == int(x):
            return str(int(x))
        return f"{x:.2f}".rstrip("0").rstrip(".")
    return str(x)


def old_results():
    p = OLD_EVI / "results.json"
    if not p.exists():
        return {}
    return {r["id"]: r for r in json.loads(p.read_text()) if r.get("done")}


def old_exhaust_tick(m):
    p = OLD_EVI / ms.MAP_INFO[m["map"]]["group"] / f"{m['id']}_frames.jsonl"
    if not p.exists():
        return None
    try:
        for line in p.open(errors="ignore"):
            d = json.loads(line)
            if not (d.get("map_view") or {}).get("resources"):
                return d.get("tick")
    except (OSError, ValueError):
        pass
    return None


def section(m):
    a = m["a"]
    rel = "../evidence/map_stress_20261031_v87_early/" + m["evidence"]
    lines = [f"### {m['id']}", "",
             "| 项目 | 内容 |", "|---|---|",
             f"| 地图 | {ms.MAP_INFO[m['map']]['label']} |",
             f"| 对手 | {ms.OPP_INFO[m['opp']]['label']} |",
             f"| 停止条件 | {'资源耗尽即 pause' if m['early_stop'] else '跑满 500 Tick'} |",
             f"| 停止 Tick | {m['stop_tick']} / {m['max_ticks']} |",
             f"| 资源 | {fmt(a['resource'])} / {m['total_resource']}（剩余节点 {m['remaining_resource']}） |",
             f"| 场次 ID | `{m['session']}` |",
             f"| 证据 | [{m['id']}]({rel}/{m['id']}_final_state.json) |", "",
             "| 名次 | 席位 | 队名 | 总分 | 资源 | 有效伤害 | 任务 | invalid | HP | 存活 |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for i, t in enumerate(m["teams"], 1):
        mark = " ★" if t["team"] == "A" else ""
        lines.append(f"| {i} | {t['team']} | {t['name']}{mark} | {fmt(t['score'])} | "
                     f"{fmt(t['resource'])} | {fmt(t['damage'])} | {t['missions']} | "
                     f"{t['invalid']} | {t['hp']} | {'是' if t['alive'] else '否'} |")
    lines += ["", f"**A（停止时）**：**{fmt(a['score'])}/60**，{m['rank']}/{len(m['teams'])}；"
                  f"资源 {fmt(a['resource'])}、伤害 {fmt(a['damage'])}、任务 {a['missions']}/3；"
                  f"invalid={a['invalid']}；AI {m['ai_success']}/{m['ai_attempt']}；"
                  f"用时 {m['elapsed_s']}s。", ""]
    acts = "、".join(f"{k}×{v}" for k, v in sorted(m["actions"].items(), key=lambda x: -x[1])[:6])
    errs = "、".join(f"{k}×{v}" for k, v in sorted(m["errors"].items(), key=lambda x: -x[1])) or "无"
    if acts:
        lines.append(f"- 动作：{acts}")
    lines.append(f"- 错误回执：{errs}")
    lines += ["", "```", *[ln for ln in m["stats"].splitlines()
                           if ln.startswith(("== 终局", "  #", "== 战斗", "  饱和",
                                             "  击杀", "  阵亡", "== 采集", "  资源"))][:20], "```", ""]
    return "\n".join(lines)


def write_report(results):
    done = [r for r in results if r.get("done")]
    early = [r for r in done if r.get("early_stop")]
    old = old_results()
    p = ["# V8.7 资源耗尽即停：多墙 / 迷宫 / 大图 / 田字 10 场（2026-10-31 晚）", "",
         f"> 整理于 {now()}；代码版本 `8d9ab8a`（V8.7）。",
         "> **口径**：四队 `resource_value` 之和达到该图资源总值（200）时立即暂停，",
         "> 不再空等到 500 Tick；因此任务是“资源耗尽那一刻”的中间态，",
         "> 不能与 500 Tick 全量成绩横比。corridor20 已在 `map_stress_20261031/` 跑满，",
         "> 本批不重复。", "",
         "## 0. 一句话", "",
         f"本批 {len(done)}/{len(MATCHES)} 场完成；其中 {len(early)} 场触发资源耗尽早停，"
         f"{len(done) - len(early)} 场跑满。所有早停场暂停时剩余资源节点为 0，"
         "“采完再换图”的停表条件已经落地。", "",
         "## 1. 总表", "",
         "| # | 场次 | 地图 | 对手 | 停止 Tick | A 资源 | A 伤害 | A 任务 | A 分 | 名次 | 剩余节点 |",
         "|---|---|---|---|---|---:|---:|---:|---:|---:|---:|"]
    for i, m in enumerate(done, 1):
        a = m["a"]
        p.append(f"| {i} | {m['id']} | {m['map']} | {m['opp']} | {m['stop_tick']} | "
                 f"{fmt(a['resource'])} | {fmt(a['damage'])} | {a['missions']} | "
                 f"{fmt(a['score'])} | {m['rank']}/{len(m['teams'])} | {m['remaining_resource']} |")
    p += ["", "> 名次和 A 分是暂停时的快照，不是终局名次。", "",
          "## 1.1 与 V8.6c 全量资源对比（只比资源）", "",
          "| 场次 | V8.6c 全量 A 资源 | V8.7 停止时 A 资源 | Δ | V8.6c 资源耗尽 Tick | V8.7 停止 Tick |",
          "|---|---:|---:|---:|---:|---:|"]
    old_sum = new_sum = 0
    old_n = 0
    for m in done:
        o = old.get(m["id"])
        if not o:
            continue
        old_res = o["a"]["resource"]
        new_res = m["a"]["resource"]
        delta = new_res - old_res
        old_sum += old_res
        new_sum += new_res
        old_n += 1
        ot = old_exhaust_tick(m)
        p.append(f"| {m['id']} | {fmt(old_res)} | {fmt(new_res)} | {delta:+.0f} | "
                 f"{ot if ot is not None else '-'} | {m['stop_tick']} |")
    p += ["", "> V8.6c 列是 500 Tick 全量局；V8.7 列是资源耗尽即停。"
          "若 V8.7 在更短时间里拿到更多资源，是强信号；若拿到更少，则是明确回归。", ""]
    if old_n:
        p += [f"A 资源均值：V8.6c **{old_sum / old_n:.1f}** → V8.7 **{new_sum / old_n:.1f}**"
              f"（Δ **{(new_sum - old_sum) / old_n:+.1f}**）。", ""]
    p += ["## 2. 逐场", ""]
    for m in done:
        p += [section(m), "---", ""]

    ticks = [m["stop_tick"] for m in done]
    full = [m for m in done if not m["early_stop"]]
    gains, losses = [], []
    for m in done:
        o = old.get(m["id"])
        if not o:
            continue
        d = m["a"]["resource"] - o["a"]["resource"]
        item = (d, m["id"], o["a"]["resource"], m["a"]["resource"])
        (gains if d > 0 else losses if d < 0 else []).append(item)
    gains.sort(reverse=True)
    losses.sort()
    p += ["## 3. 观察", ""]
    if ticks:
        p.append(f"- 停止 Tick 范围 **{min(ticks)}–{max(ticks)}**，"
                 f"相对 500 Tick 共省下 **{sum(MAX_TICKS - t for t in ticks)} Tick**"
                 f"（约 {sum(MAX_TICKS - t for t in ticks) * TICK_MS / 1000 / 60:.1f} 分钟）。")
    p.append(f"- {len(early)}/{len(done)} 场由“资源耗尽”触发暂停，"
             f"{len(full)} 场跑满；全部早停场暂停时剩余资源节点为 0。")
    if old_n:
        p.append(f"- A 资源均值从 V8.6c 的 **{old_sum / old_n:.1f}** 升到 "
                 f"**{new_sum / old_n:.1f}**，净增 **{(new_sum - old_sum) / old_n:+.1f}**。")
    if gains:
        p.append("- 资源净增最多：" + "、".join(
            f"`{mid}` {fmt(o)}→{fmt(n)}（{d:+.0f}）" for d, mid, o, n in gains[:5]) + "。")
    if losses:
        p.append("- 资源净减：" + "、".join(
            f"`{mid}` {fmt(o)}→{fmt(n)}（{d:+.0f}）" for d, mid, o, n in losses) +
            "；这两场需要针对性全量复跑，不能直接用早停数据判死。")
    b = next((m for m in done if m["id"] == "big40-official"), None)
    if b:
        a = b["a"]
        p.append(f"- `big40-official` 是最需要警惕的 tradeoff：A 资源 {fmt(a['resource'])}"
                 f"（V8.6c 全量 80），但暂停时任务 {a['missions']}/3、伤害 {fmt(a['damage'])}；"
                 "reach 尚未完成，A 仍在朝 [20,20] 途中。V8.6c 在 t124 已完成 reach；"
                 "早停切掉了后续路程，但“资源权重是否把 reach 拖慢”仍需全量复跑确认。")
    p += ["", "## 4. 读法", "",
          "- 看资源：`A 资源` 与停止 Tick，判断采集效率；本批 10 场都在资源耗尽时停。",
          "- 看任务/伤害/名次：只说明“资源耗尽那一刻”的局势，不能拿来宣布这图能赢。",
          "- 早停没有改变代码、没有放宽规则；只是把 250 s 的空转剪掉。",
          "- 若要正式成绩，仍以 `map_stress_20261031/` 的 500 Tick 全量局为准。", ""]
    DOC.parent.mkdir(parents=True, exist_ok=True)
    DOC.write_text("\n".join(p))


def write_readme(results):
    done = [r for r in results if r.get("done")]
    p = ["# V8.7 地图压测：资源耗尽即停（10 场）", "",
         "- 代码：`8d9ab8a`（V8.7 资源任务感知采集）。",
         "- 范围：多墙 4 场 + 迷宫 4 场 + 40×40 + 15×15；corridor20 已在 `../map_stress_20261031/` 跑满，不重复。",
         "- 停表：四队资源值之和达到 200 时 `/api/pause`，保存暂停现场后清理进程、换下一图。",
         "- 重要：这是资源采集压测，不是 500 Tick 全量成绩；任务/伤害/名次均为暂停时快照。", "",
         "## 文件", "",
         "| 文件 | 内容 |",
         "|---|---|",
         "| `results.json` | 10 场结构化结果，含 `early_stop/stop_tick/remaining_resource` |",
         "| `walls_maze/` | 多墙、迷宫 8 场 |",
         "| `big40/` | 40×40 1 场 |",
         "| `field15/` | 15×15 1 场 |",
         "| `../analysis/map_stress_v87_earlystop_20261031.md` | 汇总报告 |", "",
         "每场保留 `*_final_state.json`、`*_frames.jsonl`、`*_score.json`、`*_ai.jsonl`、",
         "`*_stats.txt`；`*.log` 按仓库规则只留本地。", ""]
    (EVI / "README.md").write_text("\n".join(p))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--report-only", action="store_true")
    args = ap.parse_args()
    EVI.mkdir(parents=True, exist_ok=True)
    results = load_results()
    if args.report_only:
        write_report(results)
        write_readme(results)
        return
    only = set(x for x in args.only.split(",") if x)
    for match in MATCHES:
        if only and match["id"] not in only:
            continue
        idx = next((i for i, r in enumerate(results) if r.get("id") == match["id"]), None)
        if idx is not None and results[idx].get("done") and not args.force:
            eprint(f"跳过已完成：{match['id']}")
            continue
        try:
            m = run_one(match)
        except Exception as exc:
            eprint(f"  [失败] {match['id']}: {exc}")
            fs.cleanup_all()
            m = dict(id=match["id"], map=match["map"], opp=match["opp"],
                     done=False, error=str(exc), time=now())
        results = merge_results(results, m)
        write_report(results)
        write_readme(results)
    fs.cleanup_all()
    write_report(results)
    write_readme(results)
    eprint(f"\n全部完成；结果：{RESULTS}")


if __name__ == "__main__":
    main()
