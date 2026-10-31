#!/usr/bin/env python3
# gen_maps.py —— 对抗性练习地图生成器（正式赛地图特化备战用）
#
# 用法: python3 tools/gen_maps.py [referee_stage2.json] [输出目录]
#   默认: 裁判包 ~/Desktop/dist/rmtc_practice/config/stage2.json → rmtc_blackout/maps/
# 产物: maps/map_walls.json   四面厚墙（2 格厚环墙 + 四边各 2 格宽门，逼走咽喉）
#       maps/map_maze.json    梳齿迷宫（5 道全高竖墙错位开缝，蛇形通道）
# 自检（生成即断言，任一失败退出码非 0）:
#   1. 障碍不压出生点/资源/任务点、不越界、无重复
#   2. 每个出生点到任务点与全部资源 BFS 可达（4 邻域、障碍阻挡）
#   3. 任务点不在障碍上
#
# 设计说明见 docs/analysis/map_hardening_20261029.md（生成后由分析流程补写）。

import json
import sys
from collections import deque
from pathlib import Path

REF_STAGE2 = Path.home() / "Desktop/dist/rmtc_practice/config/stage2.json"
OUT_DIR = Path(__file__).resolve().parent.parent / "maps"


def neighbors(x, y, w, h):
    for nx, ny in ((x, y - 1), (x, y + 1), (x - 1, y), (x + 1, y)):
        if 0 <= nx < w and 0 <= ny < h:
            yield nx, ny


def reachable(blocked, w, h, sx, sy, tx, ty):
    if (sx, sy) == (tx, ty):
        return True
    seen = {(sx, sy)}
    q = deque([(sx, sy)])
    while q:
        x, y = q.popleft()
        for nx, ny in neighbors(x, y, w, h):
            if (nx, ny) in blocked or (nx, ny) in seen:
                continue
            if (nx, ny) == (tx, ty):
                return True
            seen.add((nx, ny))
            q.append((nx, ny))
    return False


def walls_map():
    """四面厚墙：y=3,4 与 y=13,14 两行、x=3,4 与 x=13,14 两列是 2 格厚墙，
    每边留 x=8,9（上下）/ y=8,9（左右）双格门。角出生点在墙外走廊。"""
    obs = set()
    for y in (3, 4, 13, 14):
        for x in range(1, 17):
            if x not in (8, 9):
                obs.add((x, y))
    for x in (3, 4, 13, 14):
        for y in range(1, 17):
            if y not in (8, 9):
                obs.add((x, y))
    resources = [
        # 墙外走廊普通资源（四角 + 四边中点带）
        ("n01", 2, 1, 10), ("n02", 15, 1, 10), ("n03", 1, 15, 10), ("n04", 16, 15, 10),
        ("n05", 2, 7, 10), ("n06", 15, 7, 10), ("n07", 7, 2, 10), ("n08", 7, 15, 10),
        # 墙外高价值（左右门外的中点带）
        ("h01", 2, 9, 20), ("h02", 15, 9, 20),
        # 环内普通 + 高价值（逼进环）
        ("n09", 6, 6, 10), ("n10", 11, 6, 10), ("n11", 6, 11, 10), ("n12", 11, 11, 10),
        ("h03", 7, 9, 20), ("h04", 10, 9, 20),
    ]
    return obs, resources, [9, 9], "练习任务：收集两个普通资源、一个高价值资源，并到达 [9,9]（四面厚墙地图）。"


def maze_map():
    """梳齿迷宫：x=3/6/9/12/15 五道全高竖墙，错位各留一个 1 格缝
    （y=5/12/9/13/7），形成蛇形单通道。x=9 的缝取 y=9，让地图中心 [9,9]
    保持开阔（天下无双堡垒 center_mode=map_center 落点在中心）。任务点在 [8,8]。"""
    gaps = {3: 5, 6: 12, 9: 9, 12: 13, 15: 7}
    obs = set()
    for x, gap_y in gaps.items():
        for y in range(1, 17):
            if y != gap_y:
                obs.add((x, y))
    resources = [
        ("n01", 1, 3, 10), ("n02", 2, 7, 10), ("n03", 1, 11, 10), ("n04", 2, 14, 10),
        ("n05", 4, 2, 10), ("n06", 7, 3, 10), ("n07", 7, 11, 10), ("n08", 10, 2, 10),
        ("n09", 13, 3, 10), ("n10", 14, 8, 10), ("n11", 16, 2, 10), ("n12", 17, 14, 10),
        ("h01", 5, 6, 20), ("h02", 8, 15, 20), ("h03", 11, 13, 20), ("h04", 13, 11, 20),
    ]
    # 事件拾取点移到开阔格（校验器禁止事件点压障碍）
    event_patch = {
        "angel": {"points": [[4, 9], [14, 9]]},
        "pepper": {"points": [[4, 5], [11, 12]]},
    }
    return obs, resources, [8, 8], \
        "练习任务：收集两个普通资源、一个高价值资源，并到达 [8,8]（迷宫地图）。", event_patch


def build_config(base, obs, resources, reach, mission_text, event_patch=None):
    cfg = json.loads(json.dumps(base))  # 深拷贝
    cfg["map"]["obstacles"] = sorted([list(p) for p in obs])
    cfg["map"]["resources"] = [
        {"id": rid, "position": [x, y], "value": v} for rid, x, y, v in resources
    ]
    cfg["mission_text"] = mission_text
    for m in cfg["missions"]:
        if m["type"] == "reach":
            m["position"] = reach
    if event_patch:
        for ev, fields in event_patch.items():
            cfg["events"]["definitions"][ev].update(fields)
    return cfg


def check(cfg, name):
    w, h = cfg["map"]["width"], cfg["map"]["height"]
    obs = {tuple(p) for p in cfg["map"]["obstacles"]}
    spawns = {tuple(r["spawn"]) for r in cfg["robots"]}
    res_pts = [tuple(r["position"]) for r in cfg["map"]["resources"]]
    reach = next(m["position"] for m in cfg["missions"] if m["type"] == "reach")
    reach = tuple(reach)

    assert len(obs) == len(cfg["map"]["obstacles"]), f"{name}: 障碍重复"
    for p in obs:
        assert 0 <= p[0] < w and 0 <= p[1] < h, f"{name}: 障碍越界 {p}"
        assert p not in spawns, f"{name}: 障碍压出生点 {p}"
        assert p not in res_pts, f"{name}: 障碍压资源 {p}"
        assert p != reach, f"{name}: 障碍压任务点 {p}"
    # 事件拾取点/交点不得压在障碍上（裁判校验器同款规则）
    for ev, keys in (("angel", ("points",)), ("pepper", ("points",)),
                     ("world_split", ("point",))):
        d = cfg["events"]["definitions"].get(ev, {})
        pts = d.get(keys[0], [])
        pts = [pts] if isinstance(pts, (list,)) and pts and all(
            isinstance(v, (int, float)) for v in pts) else pts
        for p in pts:
            if isinstance(p, (list, tuple)) and len(p) == 2:
                assert tuple(p) not in obs, f"{name}: 事件 {ev} 点位 {p} 压障碍"
    for s in spawns:
        assert s not in obs, f"{name}: 出生点被墙围"
        for t in res_pts:
            assert reachable(obs, w, h, s[0], s[1], t[0], t[1]), \
                f"{name}: 出生点 {s} 到资源 {t} 不可达"
        assert reachable(obs, w, h, s[0], s[1], reach[0], reach[1]), \
            f"{name}: 出生点 {s} 到任务点 {reach} 不可达"
    print(f"  [{name}] 自检通过: {w}x{h} 障碍 {len(obs)} 资源 {len(res_pts)}"
          f" 任务点 {list(reach)}")


def main():
    base_path = Path(sys.argv[1]) if len(sys.argv) > 1 else REF_STAGE2
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else OUT_DIR
    base = json.loads(base_path.read_text())
    out_dir.mkdir(parents=True, exist_ok=True)

    for name, maker in (("map_walls", walls_map), ("map_maze", maze_map)):
        made = maker()
        obs, resources, reach, text = made[0], made[1], made[2], made[3]
        event_patch = made[4] if len(made) > 4 else None
        cfg = build_config(base, obs, resources, reach, text, event_patch)
        check(cfg, name)
        out = out_dir / f"{name}.json"
        out.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n")
        print(f"  已生成 {out}")


if __name__ == "__main__":
    main()
