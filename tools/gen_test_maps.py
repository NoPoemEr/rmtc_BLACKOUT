#!/usr/bin/env python3
"""gen_test_maps.py —— 全流程测试套件地图生成器（10/31）

产物（maps/ 下）：
  test_slight_stage1.json / test_slight_stage2.json  官方练习图 + 14 格零散短墙（约 6% 格）
  test_maze_stage1.json   / test_maze_stage2.json    梳齿迷宫（5 道全高竖墙、错位单格缝）

约束（生成即断言，失败退出码非 0）：
  - 障碍不压出生点/资源/任务点/事件点位、不越界、无重复
  - 每个出生点到全部资源与 reach 任务点 BFS 可达
  - 最后调用裁判自带 bin/validate_config 做官方口径复验

用法: python3 tools/gen_test_maps.py
"""
import json
import subprocess
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRACTICE = Path.home() / "Desktop/dist/rmtc_practice"
OUT = ROOT / "maps"

# 官方练习图（18x18）上额外加的零散短墙：避开全部 16 个资源点、4 个出生点、中心 [9,9]
SLIGHT_EXTRA = [
    (8, 6), (8, 7), (9, 7),          # 上中短墙（资源 (8,4)/(5,5)/(12,5) 之外）
    (11, 8), (12, 8),                # 右中短墙
    (6, 10), (7, 10), (7, 11),       # 左中短墙
    (14, 11), (14, 12),              # 右下短墙
    (4, 14), (5, 14),                # 左下短墙
    (10, 3), (11, 3),                # 上右短墙
    # 沿 10/31 实测 stage1 采集主路线补的 2 格短墙，确保"稍有障碍"确实改变路线
    (4, 4), (4, 5), (8, 5), (9, 5), (12, 2), (15, 4), (15, 5),
    (10, 13), (5, 13), (4, 13), (1, 14), (1, 13), (2, 8), (3, 8), (16, 8), (16, 7),
]


def neighbors(x, y, w, h):
    for nx, ny in ((x, y - 1), (x, y + 1), (x - 1, y), (x + 1, y)):
        if 0 <= nx < w and 0 <= ny < h:
            yield nx, ny


def reachable(blocked, w, h, src, dst):
    if src == dst:
        return True
    seen = {src}
    q = deque([src])
    while q:
        cur = q.popleft()
        for nxt in neighbors(*cur, w, h):
            if nxt in blocked or nxt in seen:
                continue
            if nxt == dst:
                return True
            seen.add(nxt)
            q.append(nxt)
    return False


def build(base, obstacles, resources, reach, mission_text, event_patch=None):
    cfg = json.loads(json.dumps(base))
    cfg["map"]["obstacles"] = sorted([list(p) for p in obstacles])
    cfg["map"]["resources"] = [{"id": rid, "position": [x, y], "value": v}
                               for rid, x, y, v in resources]
    cfg["mission_text"] = mission_text
    if cfg.get("missions"):
        for m in cfg["missions"]:
            if m["type"] == "reach":
                m["position"] = list(reach)
    if event_patch:
        cfg["events"].setdefault("definitions", {})
        for ev, fields in event_patch.items():
            cfg["events"]["definitions"].setdefault(ev, {}).update(fields)
    return cfg


def validate(name, cfg, spawn_count):
    w, h = cfg["map"]["width"], cfg["map"]["height"]
    obs = {tuple(p) for p in cfg["map"]["obstacles"]}
    spawns = [tuple(r["spawn"]) for r in cfg["robots"]]
    res = [tuple(r["position"]) for r in cfg["map"]["resources"]]
    assert len(spawns) == spawn_count, f"{name}: 席位 {len(spawns)} != {spawn_count}"
    assert len(obs) == len(cfg["map"]["obstacles"]), f"{name}: 障碍重复"
    for p in obs:
        assert 0 <= p[0] < w and 0 <= p[1] < h, f"{name}: 障碍越界 {p}"
        assert p not in spawns, f"{name}: 障碍压出生点 {p}"
        assert p not in res, f"{name}: 障碍压资源 {p}"
    reach = None
    for m in cfg.get("missions") or []:
        if m["type"] == "reach":
            reach = tuple(m["position"])
            assert reach not in obs, f"{name}: 障碍压任务点 {reach}"
    for ev, key in (("angel", "points"), ("pepper", "points")):
        for p in cfg.get("events", {}).get("definitions", {}).get(ev, {}).get(key, []) or []:
            assert tuple(p) not in obs, f"{name}: 事件 {ev} 点位 {p} 压障碍"
    ws = cfg.get("events", {}).get("definitions", {}).get("world_split", {})
    if ws.get("point"):
        assert tuple(ws["point"]) not in obs, f"{name}: 世界分裂点位压障碍"
    for s in spawns:
        for r in res:
            assert reachable(obs, w, h, s, r), f"{name}: {s} 到资源 {r} 不可达"
        if reach:
            assert reachable(obs, w, h, s, reach), f"{name}: {s} 到任务点 {reach} 不可达"
    # 官方校验器
    cp = subprocess.run([str(PRACTICE / "bin/validate_config")],
                        input=json.dumps(cfg), capture_output=True, text=True)
    assert cp.returncode == 0, f"{name}: 官方校验失败 {cp.stderr.strip()}"
    print(f"  [{name}] OK: {w}x{h} 障碍 {len(obs)} 资源 {len(res)} 席位 {len(spawns)}")


def main():
    s1 = json.loads((PRACTICE / "config/stage1.json").read_text())
    s2 = json.loads((PRACTICE / "config/stage2.json").read_text())
    maze = json.loads((ROOT / "maps/map_maze.json").read_text())

    # 资源布局两种图保持与官方一致，避免"资源位置变化"混入变量
    res = [(r["id"], r["position"][0], r["position"][1], r["value"])
           for r in s2["map"]["resources"]]
    slight_obs = [tuple(p) for p in s2["map"]["obstacles"]] + SLIGHT_EXTRA
    maze_obs = [tuple(p) for p in maze["map"]["obstacles"]]
    maze_res = [(r["id"], r["position"][0], r["position"][1], r["value"])
                for r in maze["map"]["resources"]]
    event_patch = {
        "angel": {"points": [[4, 9], [14, 9]]},
        "pepper": {"points": [[4, 5], [11, 12]]},
    }

    # 第二阶段：4 席位，reach=[9,9]（官方图）/ [8,8]（迷宫，同 map_maze）
    cfg = build(s2, slight_obs, res, (9, 9),
                "练习任务：收集两个普通资源、一个高价值资源，并到达 [9,9]（零散短墙地图）。")
    validate("slight_stage2", cfg, 4)
    (OUT / "test_slight_stage2.json").write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n")

    cfg = build(maze, maze_obs, maze_res, (8, 8),
                "练习任务：收集两个普通资源、一个高价值资源，并到达 [8,8]（迷宫地图）。", event_patch)
    validate("maze_stage2", cfg, 4)
    (OUT / "test_maze_stage2.json").write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n")

    # 第一阶段：仅 A 席位、无任务、无事件（裁判 start() 也会强制，这里显式写清）
    for label, obs, resources, patch in (("slight", slight_obs, res, None),
                                         ("maze", maze_obs, maze_res, event_patch)):
        cfg = build(s1, obs, resources, (9, 9), "练习采集：自主收集资源。", patch)
        cfg["stage"] = 1
        cfg["missions"] = []
        cfg["events"]["enabled"] = False
        validate(f"{label}_stage1", cfg, 1)
        (OUT / f"test_{label}_stage1.json").write_text(
            json.dumps(cfg, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
