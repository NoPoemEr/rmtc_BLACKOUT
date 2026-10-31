#!/usr/bin/env python3
"""gen_stress_maps.py —— 11 场压测用的三张自定义地图生成器（10/31 晚）

产物（maps/）：
  stress_big40.json    40×40 双层回字厚墙（多墙）+ 16 资源
  stress_field15.json  15×15 田字墙（十字墙只在中心相通）+ 16 资源
  stress_corridor20.json 20×20 四道横贯“一”字长墙 + 三段短墙 + 16 资源

所有图沿用 stage2 的四席位、三任务（2 普通 / 1 高价值 / reach）与事件；
事件几何按新图尺寸重排，保证不压障碍。生成即 BFS 校验全部出生点到资源/
reach 可达，并调用裁判自带 validate_config 做官方口径复验。

用法: python3 tools/gen_stress_maps.py
"""
import json
import subprocess
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRACTICE = Path.home() / "Desktop/dist/rmtc_practice"
BASE = PRACTICE / "config/stage2.json"
OUT = ROOT / "maps"


def add(obs, cells):
    obs.update((int(x), int(y)) for x, y in cells)


def ring(obs, x0, x1, y0, y1):
    """2 格厚矩形墙（四边各两格）。"""
    for x in range(x0, x1 + 1):
        for y in (y0, y0 + 1, y1 - 1, y1):
            obs.add((x, y))
    for y in range(y0, y1 + 1):
        for x in (x0, x0 + 1, x1 - 1, x1):
            obs.add((x, y))


def carve(obs, cells):
    for p in cells:
        obs.discard((int(p[0]), int(p[1])))


def big40():
    obs = set()
    # 外圈 x=7..32 / y=7..32，内圈 x=14..25 / y=14..25，均 2 格厚。
    ring(obs, 7, 32, 7, 32)
    ring(obs, 14, 25, 14, 25)
    # 外圈四门（2 格宽）在正中；内圈四门错位 2 格，逼车辆在夹层里换向。
    carve(obs, [(x, y) for x in (19, 20) for y in (7, 8, 31, 32)])
    carve(obs, [(x, y) for y in (19, 20) for x in (7, 8, 31, 32)])
    carve(obs, [(x, y) for x in (17, 18) for y in (14, 15, 24, 25)])
    carve(obs, [(x, y) for y in (17, 18) for x in (14, 15, 24, 25)])

    resources = [
        # 外圈走廊（8 普通）
        ("o_n01", 2, 2, 10), ("o_n02", 37, 2, 10),
        ("o_n03", 2, 37, 10), ("o_n04", 37, 37, 10),
        ("o_n05", 2, 20, 10), ("o_n06", 37, 20, 10),
        ("o_n07", 20, 2, 10), ("o_n08", 20, 37, 10),
        # 外圈与内圈之间（4 普通 + 4 高价值）
        ("m_n01", 10, 10, 10), ("m_n02", 29, 10, 10),
        ("m_n03", 10, 29, 10), ("m_n04", 29, 29, 10),
        ("m_h01", 10, 20, 20), ("m_h02", 29, 20, 20),
        ("m_h03", 20, 10, 20), ("m_h04", 20, 29, 20),
    ]
    spawns = [(1, 1), (38, 1), (1, 38), (38, 38)]
    return obs, resources, spawns, (20, 20), 40, 40, \
        "练习任务：收集两个普通资源、一个高价值资源，并到达 [20,20]（40×40 双层回字多墙地图）。"


def field15():
    obs = set()
    # 十字墙：x=7 与 y=7 各一整条，中心 3×3 开口（四向都能进出）。
    add(obs, [(7, y) for y in range(1, 14) if y not in (6, 7, 8)])
    add(obs, [(x, 7) for x in range(1, 14) if x not in (6, 7, 8)])
    resources = [
        # 西北
        ("nw_n01", 1, 2, 10), ("nw_n02", 4, 1, 10),
        ("nw_n03", 2, 4, 10), ("nw_h01", 4, 4, 20),
        # 东北
        ("ne_n01", 13, 2, 10), ("ne_n02", 10, 1, 10),
        ("ne_n03", 12, 4, 10), ("ne_h01", 10, 4, 20),
        # 西南
        ("sw_n01", 1, 12, 10), ("sw_n02", 4, 13, 10),
        ("sw_n03", 2, 10, 10), ("sw_h01", 4, 10, 20),
        # 东南
        ("se_n01", 13, 12, 10), ("se_n02", 10, 13, 10),
        ("se_n03", 12, 10, 10), ("se_h01", 10, 10, 20),
    ]
    spawns = [(2, 2), (12, 2), (2, 12), (12, 12)]
    return obs, resources, spawns, (7, 7), 15, 15, \
        "练习任务：收集两个普通资源、一个高价值资源，并到达 [7,7]（15×15 田字墙地图）。"


def corridor20():
    obs = set()
    # 四道横贯全图的“一”字墙，缺口左右交替，形成蛇形长廊。
    for y, gap in ((4, 2), (8, 17), (12, 2), (16, 17)):
        add(obs, [(x, y) for x in range(1, 19) if x != gap])
    # 三段局部短墙，增加“长廊里的折返”，不封死任何区域。
    add(obs, [(x, 6) for x in range(5, 10)])
    add(obs, [(x, 10) for x in range(10, 15)])
    add(obs, [(x, 14) for x in range(5, 10)])

    resources = [
        # 顶部走廊
        ("t_n01", 2, 1, 10), ("t_n02", 16, 1, 10), ("t_h01", 14, 2, 20),
        # 第二走廊（缺口在右）
        ("a_n01", 3, 5, 10), ("a_n02", 16, 5, 10), ("a_h01", 17, 7, 20),
        # 中央走廊（缺口在左）
        ("b_n01", 4, 9, 10), ("b_n02", 15, 9, 10), ("b_h01", 14, 11, 20),
        # 第四走廊（缺口在右）
        ("c_n01", 2, 14, 10), ("c_n02", 17, 14, 10), ("c_h01", 14, 15, 20),
        # 底部走廊
        ("d_n01", 2, 18, 10), ("d_n02", 9, 18, 10), ("d_n03", 17, 18, 10),
        ("d_n04", 6, 18, 10),
    ]
    spawns = [(1, 1), (18, 1), (1, 18), (18, 18)]
    return obs, resources, spawns, (9, 9), 20, 20, \
        "练习任务：收集两个普通资源、一个高价值资源，并到达 [9,9]（20×20 一字长廊地图）。"


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


def build(spec):
    obs, resources, spawns, reach, w, h, mission_text = spec
    cfg = json.loads(BASE.read_text())
    cfg["stage"] = 2
    cfg["map"]["width"] = w
    cfg["map"]["height"] = h
    cfg["map"]["obstacles"] = sorted([list(p) for p in obs])
    cfg["map"]["resources"] = [
        {"id": rid, "position": [x, y], "value": v} for rid, x, y, v in resources
    ]
    cfg["mission_text"] = mission_text
    for m in cfg["missions"]:
        if m["type"] == "reach":
            m["position"] = list(reach)
    # 事件几何按地图尺寸重排，所有点都避开墙。
    if w == 40:
        cfg["events"]["definitions"]["explosion"]["rect"] = {
            "top_left": [3, 3], "bottom_right": [6, 6]}
        cfg["events"]["definitions"]["illusion"]["rect"] = {
            "top_left": [33, 33], "bottom_right": [36, 36]}
        cfg["events"]["definitions"]["angel"]["points"] = [[18, 2], [18, 37]]
        cfg["events"]["definitions"]["pepper"]["points"] = [[2, 18], [37, 18]]
        cfg["events"]["definitions"]["world_split"]["point"] = [20, 20]
    elif w == 15:
        cfg["events"]["definitions"]["explosion"]["rect"] = {
            "top_left": [1, 1], "bottom_right": [3, 3]}
        cfg["events"]["definitions"]["illusion"]["rect"] = {
            "top_left": [11, 11], "bottom_right": [13, 13]}
        cfg["events"]["definitions"]["angel"]["points"] = [[5, 5], [9, 9]]
        cfg["events"]["definitions"]["pepper"]["points"] = [[5, 9], [9, 5]]
        cfg["events"]["definitions"]["world_split"]["point"] = [7, 7]
    elif w == 20:
        cfg["events"]["definitions"]["explosion"]["rect"] = {
            "top_left": [2, 2], "bottom_right": [4, 4]}
        cfg["events"]["definitions"]["illusion"]["rect"] = {
            "top_left": [15, 15], "bottom_right": [17, 17]}
        cfg["events"]["definitions"]["angel"]["points"] = [[10, 2], [10, 17]]
        cfg["events"]["definitions"]["pepper"]["points"] = [[4, 6], [15, 14]]
        cfg["events"]["definitions"]["world_split"]["point"] = [9, 10]
    # 四席位按象限/四角重新落位。
    for robot, spawn in zip(cfg["robots"], spawns):
        robot["spawn"] = list(spawn)
    return cfg


def validate(name, cfg):
    w, h = cfg["map"]["width"], cfg["map"]["height"]
    obs = {tuple(p) for p in cfg["map"]["obstacles"]}
    spawns = [tuple(r["spawn"]) for r in cfg["robots"]]
    res = [tuple(r["position"]) for r in cfg["map"]["resources"]]
    assert len(obs) == len(cfg["map"]["obstacles"]), f"{name}: 障碍重复"
    assert len(spawns) == 4 and len(set(spawns)) == 4, f"{name}: 出生点异常"
    assert len(res) == 16 and len(set(res)) == 16, f"{name}: 资源数/位置异常"
    vals = [r["value"] for r in cfg["map"]["resources"]]
    assert sum(vals) == 200 and vals.count(20) == 4 and vals.count(10) == 12, \
        f"{name}: 资源经济应为 12×10 + 4×20 = 200，实际 {vals}"
    for x, y in obs:
        assert 0 <= x < w and 0 <= y < h, f"{name}: 障碍越界 {(x,y)}"
    for p in spawns + res:
        assert p not in obs, f"{name}: 实体压障碍 {p}"
    reach = next(tuple(m["position"]) for m in cfg["missions"] if m["type"] == "reach")
    assert reach not in obs, f"{name}: reach 压障碍"
    for ev, key in (("angel", "points"), ("pepper", "points")):
        for p in cfg["events"]["definitions"][ev][key]:
            assert tuple(p) not in obs, f"{name}: 事件 {ev} 点位 {p} 压障碍"
    ws = cfg["events"]["definitions"]["world_split"]["point"]
    assert tuple(ws) not in obs, f"{name}: 世界分裂点位压障碍"
    for p in spawns:
        for r in res:
            assert reachable(obs, w, h, p, r), f"{name}: {p} 到资源 {r} 不可达"
        assert reachable(obs, w, h, p, reach), f"{name}: {p} 到 reach {reach} 不可达"
    cp = subprocess.run([str(PRACTICE / "bin/validate_config")],
                        input=json.dumps(cfg), capture_output=True, text=True)
    assert cp.returncode == 0, f"{name}: 官方校验失败 {cp.stderr.strip() or cp.stdout.strip()}"
    print(f"  [{name}] OK: {w}x{h} 障碍 {len(obs)} 资源 {len(res)} 席位 {len(spawns)}")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    specs = [
        ("stress_big40", big40),
        ("stress_field15", field15),
        ("stress_corridor20", corridor20),
    ]
    for name, maker in specs:
        cfg = build(maker())
        validate(name, cfg)
        out = OUT / f"{name}.json"
        out.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n")
        print(f"  已生成 {out}")


if __name__ == "__main__":
    main()
