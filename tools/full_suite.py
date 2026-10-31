#!/usr/bin/env python3
"""full_suite.py —— 15 场全流程测试套件（逐场部署、存证、出单场报告）

场次（用户 10/31 指定）：
  1-2   正常裁判图    第一阶段 / 第二阶段（官方参考陪练 B/C/D）
  3-4   零散短墙图    第一阶段 / 第二阶段（官方参考陪练）
  5-6   梳齿迷宫图    第一阶段 / 第二阶段（官方参考陪练）
  7-9   三图 × 全风筝避战陪练（我方自定义 B/C/D：kite）
  10-12 三图 × 全疯狗猛攻陪练（dog）
  13-15 三图 × 混合陪练（B kite / C dog / D collector）

产出：
  docs/evidence/full_suite_20261031/            每场 final_state / bot 日志 / 裁判日志 / stats
  docs/evidence/full_suite_20261031/results.json 结构化结果（可断点续跑）
  docs/analysis/full_suite_20261031.md          汇总文档（自动生成，含每场报告）

用法：
  python3 tools/full_suite.py                 # 跑全部未完成场次
  python3 tools/full_suite.py --only s2-normal-kite,s2-maze-kite
  python3 tools/full_suite.py --report-only   # 只按 results.json 重新生成文档
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.request
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRACTICE = Path.home() / "Desktop/dist/rmtc_practice"
EVI = ROOT / "docs/evidence/full_suite_20261031"
DOC = ROOT / "docs/analysis/full_suite_20261031.md"
API = "http://127.0.0.1:8080"
ENV = dict(os.environ, ROS_DOMAIN_ID="27", ROS_LOCALHOST_ONLY="0",
           RMW_IMPLEMENTATION="rmw_fastrtps_cpp")

MAP_NAME = {
    "normal": "正常（官方练习图：18×18 / 6 障碍 / 16 资源）",
    "slight": "零散短墙（18×18 / 36 障碍 / 16 资源）",
    "maze": "梳齿迷宫（18×18 / 75 障碍 / 5 道全高墙错位单缝）",
}
OPP_NAME = {
    "none": "无陪练（仅 A 席位）",
    "official": "官方参考陪练 B/C/D",
    "kite": "全风筝避战（自定义 kite ×3：永不攻击，全力拉开距离）",
    "dog": "全疯狗猛攻（自定义 dog ×3：无视资源，直扑最近敌车）",
    "mixed": "混合（B kite / C dog / D collector）",
}

MATCHES = [
    dict(id="s1-normal", stage=1, map="normal", opp="none"),
    dict(id="s2-normal", stage=2, map="normal", opp="official"),
    dict(id="s1-slight", stage=1, map="slight", opp="none"),
    dict(id="s2-slight", stage=2, map="slight", opp="official"),
    dict(id="s1-maze", stage=1, map="maze", opp="none"),
    dict(id="s2-maze", stage=2, map="maze", opp="official"),
    dict(id="s2-normal-kite", stage=2, map="normal", opp="kite"),
    dict(id="s2-slight-kite", stage=2, map="slight", opp="kite"),
    dict(id="s2-maze-kite", stage=2, map="maze", opp="kite"),
    dict(id="s2-normal-dog", stage=2, map="normal", opp="dog"),
    dict(id="s2-slight-dog", stage=2, map="slight", opp="dog"),
    dict(id="s2-maze-dog", stage=2, map="maze", opp="dog"),
    dict(id="s2-normal-mixed", stage=2, map="normal", opp="mixed"),
    dict(id="s2-slight-mixed", stage=2, map="slight", opp="mixed"),
    dict(id="s2-maze-mixed", stage=2, map="maze", opp="mixed"),
]


# ---------------------------------------------------------------- HTTP
def api(path, data=None, timeout=60):
    url = API + path
    body = None if data is None else json.dumps(data).encode()
    req = urllib.request.Request(url, data=body, method="GET" if data is None else "POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.load(resp)


def get_state(timeout=5):
    return api("/api/state", timeout=timeout)


def api_down():
    try:
        get_state(timeout=2)
        return False
    except Exception:
        return True


# ---------------------------------------------------------------- 进程
def kill_group(proc, sig=signal.SIGTERM):
    if proc is None or proc.poll() is not None:
        return
    try:
        os.killpg(os.getpgid(proc.pid), sig)
    except (ProcessLookupError, PermissionError):
        pass


def pkill(pattern):
    subprocess.run(["pkill", "-f", pattern], capture_output=True)


def cleanup_all(ref_proc=None, procs=()):
    """先 SIGTERM 裁判（practice.py 会自行回收子节点），再兜底 pkill 全部练习二进制。"""
    for p in procs:
        kill_group(p)
    if ref_proc is not None:
        kill_group(ref_proc)
        try:
            ref_proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            kill_group(ref_proc, signal.SIGKILL)
    time.sleep(1)
    for pat in ("rmtc_practice/practice.py", "rmtc_practice/bin/", "spar[r]ing_bot",
                "rmtc_bot/lib/[r]mtc_bot", "rmtc_bot/lib/rmtc_bot"):
        pkill(pat)
    for _ in range(20):
        if api_down():
            break
        time.sleep(0.5)


def start_referee(stage, official, logpath):
    cmd = ["bash", "-lc",
           f"cd {PRACTICE} && exec ./start_referee.sh --stage {stage}"
           + (" --bots B,C,D" if official else "")]
    f = open(logpath, "w")
    return subprocess.Popen(cmd, env=ENV, stdout=f, stderr=subprocess.STDOUT,
                            start_new_session=True)


def wait_ready(timeout=90):
    end = time.time() + timeout
    while time.time() < end:
        try:
            st = get_state(timeout=3)
            if st.get("status", {}).get("ready"):
                return st
        except Exception:
            pass
        time.sleep(1)
    return None


def start_bot(seat, name, logpath):
    cmd = ["bash", "-lc",
           f"cd {ROOT} && exec ./run_contestant.sh {seat} '{name}'"]
    f = open(logpath, "w")
    return subprocess.Popen(cmd, env=dict(ENV, RMTC_TEAM_TOKEN=f"practice-{seat}"),
                            stdout=f, stderr=subprocess.STDOUT, start_new_session=True)


def start_sparring(seat, profile, name, logpath):
    py = (f"source /opt/ros/humble/setup.bash; export ROS_DOMAIN_ID=27; "
          f"export ROS_LOCALHOST_ONLY=0; export RMW_IMPLEMENTATION=rmw_fastrtps_cpp; "
          f"exec python3 {ROOT}/tools/sparring_bot.py --seat {seat} "
          f"--profile {profile} --name '{name}'")
    f = open(logpath, "w")
    return subprocess.Popen(["bash", "-lc", py], env=ENV, stdout=f,
                            stderr=subprocess.STDOUT, start_new_session=True)


def wait_registered(run_dir, seats, timeout=45):
    log = Path(run_dir) / "referee.log"
    end = time.time() + timeout
    need = set(seats)
    while time.time() < end:
        try:
            text = log.read_text(errors="ignore")
        except FileNotFoundError:
            text = ""
        for s in list(need):
            if re.search(rf"registered team {s} as", text):
                need.discard(s)
        if not need:
            return True
        time.sleep(1)
    return False


def wait_finished(timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            st = get_state(timeout=4)
            if st.get("status", {}).get("finished"):
                return st
        except Exception:
            pass
        time.sleep(2)
    return None


# ---------------------------------------------------------------- 指标
def parse_bot_log(path):
    actions, reasons, errors = {}, {}, {}
    hits = 0
    try:
        for line in open(path, errors="ignore"):
            m = re.search(r"本Tick动作: (\w+)(?: (\w+))?（(.*)）\s*$", line)
            if m:
                act = m.group(1)
                actions[act] = actions.get(act, 0) + 1
                why = re.split(r"[：:（]", m.group(3))[0].strip()
                reasons[why] = reasons.get(why, 0) + 1
            e = re.search(r"error=(E_[A-Z_]+)", line)
            if e:
                errors[e.group(1)] = errors.get(e.group(1), 0) + 1
            if "被命中" in line:
                hits += 1
    except FileNotFoundError:
        pass
    return dict(actions=actions, reasons=reasons, errors=errors)


def team_row(t, stage=2):
    res = t.get("resource_value", 0) or 0
    dmg = t.get("effective_damage", 0) or 0
    cap = 200 if stage == 1 else 120
    return dict(team=t["team"], name=t.get("team_name", ""), score=t.get("score", 0),
                resource=res, damage=dmg, missions=t.get("mission_completed", 0),
                invalid=t.get("invalid_actions", 0), hp=t.get("hp", 0),
                alive=t.get("alive", False),
                r_score=round(min(res / cap, 1) * (40 if stage == 1 else 30), 2),
                d_score=round(min(dmg / 500, 1) * 15, 2),
                m_score=t.get("mission_completed", 0) * 5)


def deaths_by_team(frames_path):
    """从 frames.jsonl 统计各席位 alive→dead 次数（比只看终局 hp 可靠）。"""
    prev, deaths = {}, {}
    try:
        for line in open(frames_path, errors="ignore"):
            d = json.loads(line)
            mv = d.get("map_view")
            if isinstance(mv, str):
                mv = eval(mv)  # noqa: S307 —— 练习器自己的 repr
            for r in mv.get("robots", []):
                t, alive = r["team"], r.get("alive", True)
                if prev.get(t) and not alive:
                    deaths[t] = deaths.get(t, 0) + 1
                prev[t] = alive
    except FileNotFoundError:
        pass
    return deaths


def bot_deaths(stats_txt):
    m = re.search(r"阵亡: (.*)", stats_txt)
    if not m or not m.group(1).strip():
        return []
    return [int(x) for x in re.findall(r"\d+", m.group(1))]


def collect(match, sid, run_dir, final, log_a, stats_txt, spar_logs):
    teams = [team_row(t, match["stage"]) for t in final["board"]["teams"]]
    teams.sort(key=lambda t: (-t["score"], -t["missions"], -t["damage"]))
    a = next(t for t in teams if t["team"] == "A")
    rank = teams.index(a) + 1
    logm = parse_bot_log(log_a)
    ai = final.get("ai", {}).get("A", {})
    dbt = deaths_by_team(Path(run_dir) / "frames.jsonl")
    out = dict(id=match["id"], stage=match["stage"], map=match["map"], opp=match["opp"],
               session=sid, run_dir=run_dir, teams=teams, rank=rank, a=a,
               deaths_by_team=dbt,
               deaths=([int(x) for x in re.findall(r"\d+",
                        re.search(r"阵亡: (.*)", stats_txt).group(1))]
                       if re.search(r"阵亡: (.*)", stats_txt) else []),
               actions=logm["actions"],
               reasons=logm["reasons"], errors=logm["errors"],
               ai_success=ai.get("success_count", 0), ai_attempt=ai.get("attempt_count", 0),
               stats=stats_txt, done=True, time=datetime.now().strftime("%F %T"))
    return out


# ---------------------------------------------------------------- 单场
def run_match(match):
    mid = match["id"]
    stage, mapk, opp = match["stage"], match["map"], match["opp"]
    tick_ms = 100 if stage == 1 else 500
    max_ticks = 100 if stage == 1 else 500
    official = opp == "official"
    EVI.mkdir(parents=True, exist_ok=True)
    ref_log = EVI / f"{mid}_referee.log"
    bot_log = EVI / f"{mid}_bot.log"
    spar_logs = {}
    print(f"\n===== [{mid}] 阶段{stage} · {MAP_NAME[mapk]} · {OPP_NAME[opp]} =====", flush=True)

    cleanup_all()
    ref = start_referee(stage, official, ref_log)
    st = wait_ready(120)
    if not st:
        raise RuntimeError("裁判 120 秒未就绪")
    run_dir = st.get("run_dir", "")

    body = dict(stage=stage, tick_ms=tick_ms, max_ticks=max_ticks,
                bots=["B", "C", "D"] if official else [])
    if mapk != "normal":
        body["config"] = json.loads((ROOT / f"maps/test_{mapk}_stage{stage}.json").read_text())
    res = api("/api/start", body, timeout=90)
    sid = res.get("session_id")
    st = get_state()
    run_dir = st.get("run_dir", run_dir)
    print(f"  session={sid} run_dir={run_dir}", flush=True)

    procs = []
    procs.append(start_bot("A", "BLACKOUT队", bot_log))
    if opp in ("kite", "dog", "mixed"):
        if opp == "kite":
            plan = [("B", "kite"), ("C", "kite"), ("D", "kite")]
        elif opp == "dog":
            plan = [("B", "dog"), ("C", "dog"), ("D", "dog")]
        else:
            plan = [("B", "kite"), ("C", "dog"), ("D", "collector")]
        names = {"kite": "风筝陪练", "dog": "疯狗陪练", "collector": "采集陪练"}
        for seat, prof in plan:
            lp = EVI / f"{mid}_spar_{seat}.log"
            spar_logs[seat] = lp
            procs.append(start_sparring(seat, prof, names[prof] + seat, lp))
    need = ["A"] + ([] if opp in ("none", "official") else ["B", "C", "D"])
    if not wait_registered(run_dir, need, 60):
        raise RuntimeError(f"注册超时（需 {need}）")
    print(f"  注册完成，resume", flush=True)
    api("/api/resume", {}, timeout=10)

    timeout = max(120, max_ticks * tick_ms / 1000 + 180)
    final = wait_finished(timeout)
    if not final:
        raise RuntimeError(f"对局 {timeout:.0f}s 未结束")
    print(f"  结束 tick={final['status']['tick']} 用时约 {max_ticks * tick_ms / 1000:.0f}s", flush=True)

    (EVI / f"{mid}_final_state.json").write_text(
        json.dumps(final, ensure_ascii=False, indent=1))
    for name in ("score.json", "ai.jsonl", "frames.jsonl"):
        src = Path(run_dir) / name
        if src.exists():
            subprocess.run(["cp", str(src), str(EVI / f"{mid}_{name}")])
    if stage == 1:
        t = next(x for x in final["board"]["teams"] if x["team"] == "A")
        stats_txt = ("== 采集 ==\n"
                     f"  资源 {t.get('resource_value')}/200  "
                     f"（阶段一得分 {t.get('score')}/40）  invalid={t.get('invalid_actions')}\n")
    else:
        stats = subprocess.run([sys.executable, str(ROOT / "tools/match_stats.py"),
                                run_dir, str(bot_log)], capture_output=True, text=True)
        stats_txt = stats.stdout
    (EVI / f"{mid}_stats.txt").write_text(stats_txt)
    m = collect(match, sid, run_dir, final, bot_log, stats_txt, spar_logs)

    time.sleep(1)
    cleanup_all(ref, procs)
    print(f"  存证完成：{mid}_final_state.json", flush=True)
    return m


def failed_match(match, err):
    return dict(id=match["id"], stage=match["stage"], map=match["map"],
                opp=match["opp"], done=False, error=str(err),
                time=datetime.now().strftime("%F %T"))


# ---------------------------------------------------------------- 报告
def fmt_num(x):
    if isinstance(x, float) and x == int(x):
        return str(int(x))
    return str(x)


def section(m, notes=""):
    mid = m["id"]
    if not m.get("done"):
        if "error" not in m:
            return f"## {mid}\n\n（待跑）\n"
        return (f"## {mid}\n\n**失败：** {m.get('error')}\n\n"
                f"（{m.get('time')}）\n")
    a = m["a"]
    lines = []
    lines.append(f"## {mid}")
    lines.append("")
    lines.append("| 项目 | 内容 |")
    lines.append("|---|---|")
    lines.append(f"| 阶段 | 第{'一' if m['stage'] == 1 else '二'}阶段 |")
    lines.append(f"| 地图 | {MAP_NAME[m['map']]} |")
    lines.append(f"| 陪练 | {OPP_NAME[m['opp']]} |")
    lines.append(f"| 节奏 | {'100 ms × 100 Tick（10 s）' if m['stage'] == 1 else '500 ms × 500 Tick（250 s）'} |")
    lines.append(f"| 场次 ID | `{m['session']}` |")
    lines.append("")
    lines.append("### 终局")
    lines.append("")
    lines.append("| 名次 | 席位 | 队名 | 总分 | 资源 | 有效伤害 | 任务 | invalid | HP | 存活 |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    for i, t in enumerate(m["teams"], 1):
        mark = " ★" if t["team"] == "A" else ""
        lines.append(f"| {i} | {t['team']} | {t['name']}{mark} | {fmt_num(t['score'])} | "
                     f"{fmt_num(t['resource'])} | {fmt_num(t['damage'])} | {t['missions']} | "
                     f"{t['invalid']} | {t['hp']} | {'是' if t['alive'] else '否'} |")
    lines.append("")
    cap = 200 if m["stage"] == 1 else 120
    lines.append("### 我方（A · BLACKOUT队）")
    lines.append("")
    if m["stage"] == 1:
        lines.append(f"- 总分 **{fmt_num(a['score'])}/40**，名次 1/1；资源 {fmt_num(a['resource'])}/{cap}"
                     f"（{round(min(a['resource'] / cap, 1) * 40, 2)} 分）")
    else:
        lines.append(f"- 总分 **{fmt_num(a['score'])}/60**，名次 {m['rank']}/{len(m['teams'])}；"
                     f"资源 {fmt_num(a['resource'])}/{cap}→{a['r_score']} 分、"
                     f"伤害 {fmt_num(a['damage'])}→{a['d_score']} 分、"
                     f"任务 {a['missions']}/3→{a['m_score']} 分")
    deaths = m["deaths"]
    lines.append(f"- 阵亡 {len(deaths)} 次{'：tick ' + '、'.join(map(str, deaths)) if deaths else ''}；"
                 f"invalid={a['invalid']}；AI 成功 {m['ai_success']}/{m['ai_attempt']}")
    acts = "、".join(f"{k}×{v}" for k, v in sorted(m["actions"].items(), key=lambda x: -x[1])[:6])
    errs = "、".join(f"{k}×{v}" for k, v in sorted(m["errors"].items(), key=lambda x: -x[1])) or "无"
    if acts:
        lines.append(f"- 动作分布：{acts}")
    lines.append(f"- 错误回执：{errs}")
    reasons = "、".join(f"{k}×{v}" for k, v in sorted(m["reasons"].items(),
                                                     key=lambda x: -x[1])[:5])
    if reasons:
        lines.append(f"- 决策 Top5：{reasons}")
    lines.append("")
    spar = [t for t in m["teams"] if t["team"] in ("B", "C", "D")]
    if spar:
        lines.append("### 陪练/对手表现")
        lines.append("")
        dbt = m.get("deaths_by_team") or {}
        lines.append("| 席位 | 队名 | 资源 | 有效伤害 | 任务 | 阵亡次数 | HP |")
        lines.append("|---|---|---|---|---|---|---|")
        for t in spar:
            lines.append(f"| {t['team']} | {t['name']} | {fmt_num(t['resource'])} | "
                         f"{fmt_num(t['damage'])} | {t['missions']} | "
                         f"{dbt.get(t['team'], 0)} | {t['hp']} |")
        lines.append("")
    lines.append("### stats 摘要")
    lines.append("")
    lines.append("```")
    for ln in m["stats"].splitlines():
        if ln.startswith(("== 战斗", "  饱和", "  击杀", "  阵亡", "== 交火", "  被命中",
                          "== 决策分布", "== 采集", "  资源")) or re.match(r"\s+\d+\s+(move|attack|defend|wait)", ln):
            lines.append(ln)
    lines.append("```")
    lines.append("")
    if notes.strip():
        lines.append(notes.strip())
        lines.append("")
    return "\n".join(lines)


def write_report(results):
    done = [r for r in results if r.get("done")]
    parts = []
    parts.append("# RMTC 全流程 15 场测试报告（2026-10-31）")
    parts.append("")
    parts.append(f"> 自动生成于 {datetime.now().strftime('%F %T')}；"
                 f"完成 {len(done)}/15 场。单场原始数据见 `docs/evidence/full_suite_20261031/`。")
    parts.append("")
    parts.append("## 总览")
    parts.append("")
    parts.append("| # | 场次 | 阶段 | 地图 | 陪练 | A 总分 | 名次 | 资源 | 伤害 | 任务 | 阵亡 | invalid | AI |")
    parts.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for i, r in enumerate(results, 1):
        if not r.get("done"):
            parts.append(f"| {i} | {r['id']} | - | - | - | - | - | - | - | - | - | - | 失败 |")
            continue
        a = r["a"]
        parts.append(f"| {i} | {r['id']} | {r['stage']} | {r['map']} | {r['opp']} | "
                     f"{fmt_num(a['score'])} | {r['rank']}/{len(r['teams'])} | "
                     f"{fmt_num(a['resource'])} | {fmt_num(a['damage'])} | {a['missions']} | "
                     f"{len(r['deaths'])} | {a['invalid']} | {r['ai_success']} |")
    parts.append("")
    for r in results:
        note_file = EVI / "notes" / f"{r['id']}.md"
        notes = note_file.read_text() if note_file.exists() else ""
        parts.append(section(r, notes))
        parts.append("---")
        parts.append("")
    parts.append("## 汇总结论与待调整项")
    parts.append("")
    sm = EVI / "notes" / "_summary.md"
    parts.append(sm.read_text() if sm.exists() else "（待全部跑完后填写）")
    parts.append("")
    DOC.write_text("\n".join(parts))
    print(f"报告已写入 {DOC}", flush=True)


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--report-only", action="store_true")
    ap.add_argument("--force", action="store_true", help="重跑 --only 指定的场次（覆盖旧结果）")
    args = ap.parse_args()
    EVI.mkdir(parents=True, exist_ok=True)
    (EVI / "notes").mkdir(exist_ok=True)
    res_path = EVI / "results.json"
    if res_path.exists():
        results = json.loads(res_path.read_text())
        by_id = {r["id"]: r for r in results}
        results = [by_id.get(m["id"], dict(id=m["id"], done=False)) for m in MATCHES]
    else:
        results = [dict(id=m["id"], done=False) for m in MATCHES]

    if args.report_only:
        write_report(results)
        return

    only = set(x for x in args.only.split(",") if x)
    for idx, match in enumerate(MATCHES):
        if only and match["id"] not in only:
            continue
        if results[idx].get("done") and not (args.force and match["id"] in only):
            print(f"跳过已完成：{match['id']}", flush=True)
            continue
        try:
            m = run_match(match)
        except Exception as exc:
            print(f"  [失败] {match['id']}: {exc}", flush=True)
            try:
                cleanup_all()
            except Exception:
                pass
            m = failed_match(match, exc)
        results[idx] = m
        res_path.write_text(json.dumps(results, ensure_ascii=False, indent=1))
        write_report(results)
    cleanup_all()
    write_report(results)


if __name__ == "__main__":
    main()
