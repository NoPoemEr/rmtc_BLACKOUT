#!/usr/bin/env python3
"""map_stress_suite.py —— 10/31 晚 11 场地图压测编排

场次：
  前 8 场（合并报告）：
    多墙 map_walls  × 官方 / 风筝 / 疯狗 / 混合
    迷宫 map_maze   × 官方 / 风筝 / 疯狗 / 混合
  后 3 场（各一份报告）：
    40×40 双层回字多墙 / 15×15 田字 / 20×20 一字长廊，各对官方陪练

统一节奏：stage2、500 ms × 500 Tick、A=BLACKOUT队；B/C/D 按对手类型启动。
产出：
  docs/evidence/map_stress_20261031/<group>/<id>_*.json|log|txt
  docs/evidence/map_stress_20261031/results.json
  docs/analysis/walls_maze_matrix_20261031.md
  docs/analysis/big40_20261031.md / field15_20261031.md / corridor20_20261031.md

用法:
  python3 tools/map_stress_suite.py                 # 跑未完成场次 + 重生成报告
  python3 tools/map_stress_suite.py --only walls-official,maze-dog --force
  python3 tools/map_stress_suite.py --report-only
  python3 tools/map_stress_suite.py --smoke         # 20 Tick 冒烟（不入正式结果）

注意：--force / --report-only 会重写 docs/analysis/ 下的单图报告，
      手写分析请先备份；报告里的代码版本取自当前 git HEAD。
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import full_suite as fs  # noqa: E402

EVI = ROOT / "docs/evidence/map_stress_20261031"
SMOKE_EVI = ROOT / "docs/evidence/map_stress_smoke"
DOC_WALLS = ROOT / "docs/analysis/walls_maze_matrix_20261031.md"
DOC_ONE = {
    "big40": ROOT / "docs/analysis/big40_20261031.md",
    "field15": ROOT / "docs/analysis/field15_20261031.md",
    "corridor20": ROOT / "docs/analysis/corridor20_20261031.md",
}
API = fs.API


def _code_rev():
    try:
        return subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "--short=7", "HEAD"],
            text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


CODE_REV = _code_rev()
CODE_VERSION = "V8.7"

MAP_INFO = {
    "walls": dict(
        label="多墙（18×18 / 96 障碍 / 四道 2 格厚环墙 + 双格门）",
        cfg=ROOT / "maps/map_walls.json", group="walls_maze",
        desc="四面 2 格厚墙把地图切成外走廊与内环，四边各留 2 格宽门；"
             "16 个资源 8 普通 + 4 高价值分布在外走廊与环内。"),
    "maze": dict(
        label="迷宫（18×18 / 75 障碍 / 5 道全高墙错位单缝）",
        cfg=ROOT / "maps/map_maze.json", group="walls_maze",
        desc="5 道全高竖墙各留 1 格错位缝，形成蛇形单通道；"
             "中心任务点 [8,8]，资源藏在各条墙后。"),
    "big40": dict(
        label="40×40 双层回字多墙 / 240 障碍",
        cfg=ROOT / "maps/stress_big40.json", group="big40",
        desc="外圈 x=7..32、内圈 x=14..25 的两层 2 格厚矩形墙，"
             "四门 2 格宽且内外错位；16 资源分布在外走廊与两墙之间，"
             "任务点在地图正中 [20,20]。"),
    "field15": dict(
        label="15×15 田字墙 / 20 障碍",
        cfg=ROOT / "maps/stress_field15.json", group="field15",
        desc="x=7、y=7 两道十字墙把地图分成四个 5×5 房间，"
             "中心留 3×3 十字路口；四席位各占一角房间，16 资源每房 4 个，"
             "任务点在中心 [7,7]。"),
    "corridor20": dict(
        label="20×20 一字长廊 / 83 障碍",
        cfg=ROOT / "maps/stress_corridor20.json", group="corridor20",
        desc="y=4/8/12/16 四道横贯长墙，缺口左右交替形成蛇形走廊；"
             "另有三段局部短墙制造折返；任务点 [9,9]，16 资源分散在各走廊。"),
}
OPP_INFO = {
    "official": dict(label="裁判默认（官方参考陪练 B/C/D）", official=True,
                     plan=[]),
    "kite": dict(label="全风筝（自定义 kite ×3：不主动攻击，拉距离为主）",
                 official=False, plan=[("B", "kite"), ("C", "kite"), ("D", "kite")]),
    "dog": dict(label="全疯狗（自定义 dog ×3：无视资源，直扑最近敌车）",
                official=False, plan=[("B", "dog"), ("C", "dog"), ("D", "dog")]),
    "mixed": dict(label="混合（B kite / C dog / D collector）",
                  official=False, plan=[("B", "kite"), ("C", "dog"), ("D", "collector")]),
}
MATCHES = [
    dict(id="walls-official", map="walls", opp="official"),
    dict(id="walls-kite", map="walls", opp="kite"),
    dict(id="walls-dog", map="walls", opp="dog"),
    dict(id="walls-mixed", map="walls", opp="mixed"),
    dict(id="maze-official", map="maze", opp="official"),
    dict(id="maze-kite", map="maze", opp="kite"),
    dict(id="maze-dog", map="maze", opp="dog"),
    dict(id="maze-mixed", map="maze", opp="mixed"),
    dict(id="big40-official", map="big40", opp="official"),
    dict(id="field15-official", map="field15", opp="official"),
    dict(id="corridor20-official", map="corridor20", opp="official"),
]
WALLS_IDS = {m["id"] for m in MATCHES[:8]}


def eprint(*a):
    print(*a, flush=True)


def now():
    return datetime.now().strftime("%F %T")


def run_one(match, max_ticks=500, smoke=False):
    mid = match["id"]
    info = MAP_INFO[match["map"]]
    opp = OPP_INFO[match["opp"]]
    tick_ms = 500
    root = SMOKE_EVI if smoke else EVI
    out = root / info["group"]
    out.mkdir(parents=True, exist_ok=True)
    ref_log = out / f"{mid}_referee.log"
    bot_log = out / f"{mid}_bot.log"
    eprint(f"\n===== [{mid}] {info['label']} · {opp['label']} · "
           f"{tick_ms}ms×{max_ticks} =====")

    fs.cleanup_all()
    ref = fs.start_referee(2, opp["official"], ref_log)
    st = fs.wait_ready(120)
    if not st:
        raise RuntimeError("裁判 120 秒未就绪")
    run_dir = st.get("run_dir", "")

    body = dict(stage=2, tick_ms=tick_ms, max_ticks=max_ticks,
                bots=["B", "C", "D"] if opp["official"] else [])
    body["config"] = json.loads(info["cfg"].read_text())
    res = fs.api("/api/start", body, timeout=90)
    sid = res.get("session_id")
    st = fs.get_state()
    run_dir = st.get("run_dir", run_dir)
    eprint(f"  session={sid} run_dir={run_dir}")

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

    timeout = max(120, max_ticks * tick_ms / 1000 + 180)
    final = fs.wait_finished(timeout)
    if not final:
        raise RuntimeError(f"对局 {timeout:.0f}s 未结束")
    eprint(f"  结束 tick={final['status']['tick']} "
           f"用时约 {max_ticks * tick_ms / 1000:.0f}s")

    (out / f"{mid}_final_state.json").write_text(
        json.dumps(final, ensure_ascii=False, indent=1))
    for name in ("score.json", "ai.jsonl", "frames.jsonl"):
        src = Path(run_dir) / name
        if src.exists():
            shutil.copy2(src, out / f"{mid}_{name}")

    # 保留 traceback 级别的 stderr 也在这里；stats 直接跑官方统计脚本。
    stats = subprocess.run([sys.executable, str(ROOT / "tools/match_stats.py"),
                            run_dir, str(bot_log)], capture_output=True, text=True)
    stats_txt = stats.stdout + (("\n[stderr]\n" + stats.stderr) if stats.stderr else "")
    (out / f"{mid}_stats.txt").write_text(stats_txt)

    m = collect(match, sid, run_dir, final, bot_log, stats_txt)
    time.sleep(1)
    fs.cleanup_all(ref, procs)
    eprint(f"  存证完成：{mid}_final_state.json")
    return m


def collect(match, sid, run_dir, final, bot_log, stats_txt):
    stage = 2
    teams = [fs.team_row(t, stage) for t in final["board"]["teams"]]
    teams.sort(key=lambda t: (-t["score"], -t["missions"], -t["damage"]))
    a = next(t for t in teams if t["team"] == "A")
    rank = teams.index(a) + 1
    logm = fs.parse_bot_log(bot_log)
    ai = final.get("ai", {}).get("A", {})
    dbt = fs.deaths_by_team(Path(run_dir) / "frames.jsonl")
    deaths = fs.bot_deaths(stats_txt)
    return dict(id=match["id"], map=match["map"], opp=match["opp"], session=sid,
                run_dir=run_dir, teams=teams, rank=rank, a=a,
                deaths_by_team=dbt, deaths=deaths, actions=logm["actions"],
                reasons=logm["reasons"], errors=logm["errors"],
                ai_success=ai.get("success_count", 0),
                ai_attempt=ai.get("attempt_count", 0),
                stats=stats_txt, done=True, time=now())


def fmt_num(x):
    if isinstance(x, float) and x == int(x):
        return str(int(x))
    return str(x)


def evidence_rel(m):
    info = MAP_INFO[m["map"]]
    return f"../evidence/map_stress_20261031/{info['group']}/{m['id']}"


def match_section(m):
    a = m["a"]
    lines = [f"### {m['id']}", "",
             "| 项目 | 内容 |", "|---|---|",
             f"| 地图 | {MAP_INFO[m['map']]['label']} |",
             f"| 对手 | {OPP_INFO[m['opp']]['label']} |",
             f"| 节奏 | 500 ms × 500 Tick（250 s） |",
             f"| 场次 ID | `{m['session']}` |",
             f"| 证据 | [{m['id']}]({evidence_rel(m)}_final_state.json) |", "",
             "| 名次 | 席位 | 队名 | 总分 | 资源 | 有效伤害 | 任务 | invalid | HP | 存活 |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for i, t in enumerate(m["teams"], 1):
        mark = " ★" if t["team"] == "A" else ""
        lines.append(f"| {i} | {t['team']} | {t['name']}{mark} | {fmt_num(t['score'])} | "
                     f"{fmt_num(t['resource'])} | {fmt_num(t['damage'])} | {t['missions']} | "
                     f"{t['invalid']} | {t['hp']} | {'是' if t['alive'] else '否'} |")
    lines += ["", f"**A**：**{fmt_num(a['score'])}/60**，{m['rank']}/{len(m['teams'])}；"
                  f"资源 {fmt_num(a['resource'])}/120→{a['r_score']}、"
                  f"伤害 {fmt_num(a['damage'])}/500→{a['d_score']}、"
                  f"任务 {a['missions']}/3→{a['m_score']}；"
                  f"invalid={a['invalid']}；阵亡 {len(m['deaths'])} 次"
                  + (f"（tick {'、'.join(map(str, m['deaths']))}）" if m["deaths"] else "")
                  + f"；AI {m['ai_success']}/{m['ai_attempt']}。", ""]
    dbt = m.get("deaths_by_team") or {}
    opp = [t for t in m["teams"] if t["team"] in ("B", "C", "D")]
    if opp:
        lines += ["| 对手 | 队名 | 资源 | 有效伤害 | 任务 | 阵亡 | HP |",
                  "|---|---|---|---|---|---|---|"]
        for t in opp:
            lines.append(f"| {t['team']} | {t['name']} | {fmt_num(t['resource'])} | "
                         f"{fmt_num(t['damage'])} | {t['missions']} | "
                         f"{dbt.get(t['team'], 0)} | {t['hp']} |")
        lines.append("")
    acts = "、".join(f"{k}×{v}" for k, v in sorted(m["actions"].items(), key=lambda x: -x[1])[:8])
    errs = "、".join(f"{k}×{v}" for k, v in sorted(m["errors"].items(), key=lambda x: -x[1])) or "无"
    reasons = "、".join(f"{k}×{v}" for k, v in sorted(m["reasons"].items(), key=lambda x: -x[1])[:6])
    if acts:
        lines.append(f"- 动作：{acts}")
    lines.append(f"- 错误回执：{errs}")
    if reasons:
        lines.append(f"- 决策 Top6：{reasons}")
    lines += ["", "```", *excerpt_stats(m["stats"]), "```", ""]
    return "\n".join(lines)


def excerpt_stats(stats):
    keep = []
    for ln in stats.splitlines():
        if ln.startswith(("== 战斗", "  饱和", "  击杀", "  阵亡", "== 交火",
                          "  被命中", "== 决策分布", "== 采集", "  资源")):
            keep.append(ln)
        elif re.match(r"\s+\d+\s+(move|attack|defend|wait)", ln):
            keep.append(ln)
    return keep[:45]


def matrix_table(ms):
    lines = ["| # | 场次 | 地图 | 对手 | A 分 | 名次 | 资源 | 伤害 | 任务 | 阵亡 | invalid | 对手最高 |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for i, m in enumerate(ms, 1):
        a = m["a"]
        others = [t for t in m["teams"] if t["team"] != "A"]
        best = max(others, key=lambda t: t["score"]) if others else None
        lines.append(f"| {i} | {m['id']} | {m['map']} | {m['opp']} | {fmt_num(a['score'])} | "
                     f"{m['rank']}/{len(m['teams'])} | {fmt_num(a['resource'])} | "
                     f"{fmt_num(a['damage'])} | {a['missions']} | {len(m['deaths'])} | "
                     f"{a['invalid']} | {fmt_num(best['score']) if best else '-'} |")
    return "\n".join(lines)


def write_walls_report(results):
    ms = [r for r in results if r.get("done") and r["id"] in WALLS_IDS]
    ms.sort(key=lambda r: [m["id"] for m in MATCHES].index(r["id"]))
    p = ["# 多墙 / 迷宫 × 四种对手：8 场压测报告（2026-10-31 晚）", "",
         f"> 自动生成于 {now()}；代码版本 `{CODE_REV}`（{CODE_VERSION}）。",
         "> 统一 stage2、500 ms × 500 Tick；A=BLACKOUT队，B/C/D 为指定对手。",
         "> 前 8 场原始数据：`docs/evidence/map_stress_20261031/walls_maze/`。", "",
         "## 0. 一句话", "",
         "（跑完后填写）", "",
         "## 1. 矩阵", "", matrix_table(ms), "",
         "## 2. 逐场", ""]
    for m in ms:
        p += [match_section(m), "---", ""]
    p += ["## 3. 横向结论", "", "（跑完后填写）", ""]
    DOC_WALLS.write_text("\n".join(p))
    return DOC_WALLS


def write_one_report(results, key):
    m = next((r for r in results if r.get("done") and r["id"] == f"{key}-official"), None)
    if m is None:
        return None
    info = MAP_INFO[key]
    p = [f"# {info['label']}：对裁判默认陪练单场报告（2026-10-31 晚）", "",
         f"> 自动生成于 {now()}；代码版本 `{CODE_REV}`（{CODE_VERSION}）。",
         "> 统一 stage2、500 ms × 500 Tick；A=BLACKOUT队，B/C/D=官方参考陪练。",
         f"> 原始数据：`docs/evidence/map_stress_20261031/{info['group']}/`。", "",
         "## 0. 地图与目的", "", info["desc"], "",
         f"- 地图：{info['label']}",
         f"- 资源：16 个（12 普通 ×10 + 4 高价值 ×20，总分 200）",
         "- 任务：2 普通资源 + 1 高价值资源 + 到达任务点",
         f"- 目的：检验 {CODE_VERSION} 在非官方几何下的采集/任务/战斗/生存是否稳定。", "",
         "## 1. 终局", "", match_section(m), "---", "",
         "## 2. 关键观察", "", "（跑完后填写）", "",
         "## 3. 结论与风险", "", "（跑完后填写）", ""]
    DOC_ONE[key].write_text("\n".join(p))
    return DOC_ONE[key]


def write_reports(results):
    write_walls_report(results)
    for key in ("big40", "field15", "corridor20"):
        write_one_report(results, key)


def load_results():
    p = EVI / "results.json"
    if p.exists():
        return json.loads(p.read_text())
    return []


def merge_results(results, m):
    by = {r["id"]: r for r in results}
    by[m["id"]] = m
    out = [by.get(x["id"], dict(id=x["id"], done=False)) for x in MATCHES]
    EVI.mkdir(parents=True, exist_ok=True)
    (EVI / "results.json").write_text(json.dumps(out, ensure_ascii=False, indent=1))
    return out


def smoke():
    """只跑 20 Tick，验证三张新图能部署、能注册、能结算。"""
    fs.cleanup_all()
    for match in [m for m in MATCHES if m["map"] in ("big40", "field15", "corridor20")]:
        try:
            eprint(f"[smoke] {match['id']}")
            run_one(match, max_ticks=20, smoke=True)
        except Exception as exc:
            eprint(f"[smoke][失败] {match['id']}: {exc}")
            fs.cleanup_all()
            raise
    fs.cleanup_all()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--report-only", action="store_true")
    ap.add_argument("--smoke", action="store_true")
    args = ap.parse_args()
    EVI.mkdir(parents=True, exist_ok=True)
    results = load_results()
    if args.smoke:
        smoke()
        return
    if args.report_only:
        write_reports(results)
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
        write_reports(results)
    fs.cleanup_all()
    write_reports(results)
    eprint(f"\n全部完成；结果：{EVI / 'results.json'}")


if __name__ == "__main__":
    main()
