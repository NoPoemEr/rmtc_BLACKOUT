#!/usr/bin/env python3
"""sparring_bot.py —— 练习赛自定义陪练（三档行为，纯本地规则，不调 LLM）

用途：给 BLACKOUT 主程序（A 席位）制造比官方参考陪练更强/更有针对性的对抗压力。
官方参考陪练只有一种朴素策略；本脚本按档位控制 B/C/D 任一席位：

  kite      全风筝避战：永不主动攻击，始终拉开与最近敌车距离；安全时顺路抢资源
  dog       全疯狗猛攻：完全无视资源，直扑最近可见/记忆敌车，进入 1–2 格直线立即开火
  collector 机会主义采集：优先资源，仅在射程内且占优时还手，近身且残血则撤

协议要点（CONTESTANT_PROTOCOL.md）：
  - 订阅 /team_X/perception，发布 /team_X/action，原样回填该帧 tick/session_id
  - 视野实体 position 是 9x9 相对图，'2' 相对自身 [列-5, 行-5]，绝对坐标 = self.position + 相对
  - 阵亡只能 wait；出生保护期不能攻击；攻击只打直线 1–2 格内最近敌车

用法：
  python3 tools/sparring_bot.py --seat B --profile kite --name 风筝陪练B
  python3 tools/sparring_bot.py --seat C --profile dog  --name 疯狗陪练C
  python3 tools/sparring_bot.py --seat D --profile collector --name 采集陪练D
环境：source /opt/ros/humble/setup.bash；ROS_DOMAIN_ID 与裁判一致（默认 27）。
"""
import argparse
import json
import random
import sys
from collections import deque

import rclpy
from rclpy.node import Node
from rclpy.qos import (DurabilityPolicy, HistoryPolicy, QoSProfile,
                       ReliabilityPolicy)
from std_msgs.msg import String

DIRS = {"up": (0, -1), "down": (0, 1), "left": (-1, 0), "right": (1, 0)}
OPPOSITE = {"up": "down", "down": "up", "left": "right", "right": "left"}


def parse_rel(pos_text):
    """9x9 相对位置图 -> (dx, dy)；找不到 '2' 返回 None（与自身重叠时中心保留 '1'）。"""
    if not isinstance(pos_text, str):
        return None
    rows = pos_text.split("\n")
    if len(rows) != 9:
        return None
    for ry, row in enumerate(rows):
        if len(row) != 9:
            return None
        cx = row.find("2")
        if cx >= 0:
            return cx - 4, ry - 4
    return 0, 0


class Sparring(Node):
    def __init__(self, seat, profile, name, tick_ms_hint=500):
        super().__init__(f"sparring_{seat.lower()}")
        self.seat = seat
        self.profile = profile
        self.name = name
        self.qos = QoSProfile(depth=1, reliability=ReliabilityPolicy.RELIABLE,
                              durability=DurabilityPolicy.VOLATILE,
                              history=HistoryPolicy.KEEP_LAST)
        self.reg_qos = QoSProfile(depth=1, reliability=ReliabilityPolicy.RELIABLE,
                                  durability=DurabilityPolicy.TRANSIENT_LOCAL,
                                  history=HistoryPolicy.KEEP_LAST)
        self.pub = self.create_publisher(String, f"/team_{seat}/action", self.qos)
        self.reg = self.create_publisher(String, f"/team_{seat}/registration", self.reg_qos)
        self.sub = self.create_subscription(String, f"/team_{seat}/perception",
                                            self.on_perception, self.qos)
        self.reg_msg = json.dumps({"team": seat, "name": name}, ensure_ascii=False)
        self.create_timer(1.0, self.publish_reg)
        self.tick_ms_hint = tick_ms_hint

        # 状态
        self.session = None
        self.last_tick = -1
        self.pos = None
        self.alive = True
        self.hp = 100
        self.protected = False
        self.map_w, self.map_h = 18, 18
        self.obstacles = set()          # 已知静态障碍（视野累计）
        self.resources = {}             # id -> (x, y, value)
        self.res_seen_tick = {}
        self.enemies = {}               # team -> {"pos":(x,y), "tick":t, "hp":hp}
        self.visited = set()
        self.last_seen_self = None
        self.stuck = 0
        self.flee_until = -1
        self.flee_target = None
        self.last_move_dir = None
        self.rng = random.Random(1000 + ord(seat))
        self.actions_sent = 0
        self.decisions = {}
        self.get_logger().info(f"陪练启动 seat={seat} profile={profile} name={name}")

    # ---------------- 发布 ----------------
    def publish_reg(self):
        self.reg.publish(String(data=self.reg_msg))

    def send(self, action, direction=None, why=""):
        if self.session is None or self.last_tick < 0:
            return
        msg = {"tick": self.last_tick, "session_id": self.session, "action": action}
        if direction:
            msg["direction"] = direction
        self.pub.publish(String(data=json.dumps(msg)))
        self.actions_sent += 1
        key = action if not direction else f"{action} {direction}"
        self.decisions[key] = self.decisions.get(key, 0) + 1
        if why:
            self.get_logger().info(f"tick={self.last_tick} {key}（{why}）")

    # ---------------- 感知 ----------------
    def on_perception(self, msg):
        try:
            self.process(json.loads(msg.data))
        except Exception as exc:  # 任何解析异常都降级 wait，绝不退出
            self.get_logger().warn(f"perception 解析异常: {exc!r}")
            try:
                self.send("wait", None, "解析异常降级")
            except Exception:
                pass

    def process(self, p):
        tick = p.get("tick")
        sid = p.get("session_id")
        if not isinstance(tick, int) or not isinstance(sid, str):
            return
        if sid != self.session:
            self.session = sid
            self.obstacles.clear()
            self.resources.clear()
            self.res_seen_tick.clear()
            self.enemies.clear()
            self.visited.clear()
            self.last_tick = -1
            self.stuck = 0
            self.flee_until = -1
            self.flee_target = None
        if tick <= self.last_tick:          # 同 tick 重复帧不重复决策
            return
        self.last_tick = tick

        m = p.get("map") or {}
        if isinstance(m.get("width"), int) and isinstance(m.get("height"), int):
            self.map_w, self.map_h = m["width"], m["height"]

        s = p.get("self") or {}
        pos = s.get("position")
        if isinstance(pos, list) and len(pos) == 2 and all(isinstance(v, int) for v in pos):
            self.pos = (pos[0], pos[1])
            self.visited.add(self.pos)
        self.alive = bool(s.get("alive", True))
        self.hp = int(s.get("hp", self.hp) or 0)
        self.protected = bool(s.get("protected", False))
        if s.get("position_reason") in ("initial", "respawn"):
            self.stuck = 0

        if not self.alive or self.pos is None:
            self.send("wait", None, "阵亡等待" if not self.alive else "无位置")
            return

        self.update_world(p)

        # 卡住检测：连续 3 个 tick 想走却没动
        if self.last_seen_self == self.pos:
            self.stuck += 1
        else:
            self.stuck = 0
        self.last_seen_self = self.pos

        action, direction, why = self.decide()
        if direction:
            self.last_move_dir = direction
        self.send(action, direction, why)

    def update_world(self, p):
        """从可见实体刷新障碍/资源/敌车记忆。"""
        visible_res = set()
        visible_enemies = {}
        entities = p.get("visible_entities") or []
        for e in entities:
            if not isinstance(e, dict):
                continue
            rel = parse_rel(e.get("position"))
            if rel is None:
                continue
            cell = (self.pos[0] + rel[0], self.pos[1] + rel[1])
            if not (0 <= cell[0] < self.map_w and 0 <= cell[1] < self.map_h):
                continue
            t = e.get("type")
            if t == "obstacle":
                self.obstacles.add(cell)
            elif t in ("resource_normal", "resource_high"):
                rid = str(e.get("id") or cell)
                self.resources[rid] = (cell[0], cell[1], int(e.get("value", 10) or 10))
                self.res_seen_tick[rid] = self.last_tick
                visible_res.add(rid)
            elif t == "robot":
                team = str(e.get("team") or e.get("robot_id") or "")
                if team and team != self.seat:
                    visible_enemies[team] = cell
                    self.enemies[team] = {"pos": cell, "tick": self.last_tick,
                                          "hp": int(e.get("hp", 100) or 0),
                                          "protected": bool(e.get("protected", False))}

        # 视距内的资源记忆消失 = 已被采走
        vision = int(p.get("vision_radius", 4) or 4)
        for rid, (rx, ry, _v) in list(self.resources.items()):
            if rid in visible_res:
                continue
            if abs(rx - self.pos[0]) <= vision and abs(ry - self.pos[1]) <= vision:
                self.resources.pop(rid, None)
                self.res_seen_tick.pop(rid, None)
        # 清掉太久没见、且已探过区域的敌车记忆
        for team, info in list(self.enemies.items()):
            if self.last_tick - info["tick"] > 60:
                self.enemies.pop(team, None)

    # ---------------- 通用工具 ----------------
    def free(self, cell, dynamic=True):
        x, y = cell
        if not (0 <= x < self.map_w and 0 <= y < self.map_h):
            return False
        if cell in self.obstacles:
            return False
        if dynamic:
            for team, info in self.enemies.items():
                if info["tick"] == self.last_tick and info["pos"] == cell:
                    return False
        return True

    def manhattan(self, a, b):
        return abs(a[0] - b[0]) + abs(a[1] - b[1])

    def bfs(self, start, goals, dynamic=True, avoid_enemy_radius=0, limit=4000):
        """返回从 start 到任一 goal 的最短路径（含 goal）；不可达返回 None。"""
        goals = set(goals)
        if not goals:
            return None
        if start in goals:
            return [start]
        prev = {start: None}
        q = deque([start])
        while q and len(prev) < limit:
            cur = q.popleft()
            for d, (dx, dy) in DIRS.items():
                nxt = (cur[0] + dx, cur[1] + dy)
                if nxt in prev or not self.free(nxt, dynamic):
                    continue
                if avoid_enemy_radius:
                    near = any(self.manhattan(nxt, i["pos"]) <= avoid_enemy_radius
                               for i in self.enemies.values())
                    if near and nxt not in goals:
                        continue
                prev[nxt] = cur
                if nxt in goals:
                    path = [nxt]
                    while prev[path[-1]] is not None:
                        path.append(prev[path[-1]])
                    return list(reversed(path))
                q.append(nxt)
        return None

    def first_dir(self, path):
        if not path or len(path) < 2:
            return None
        dx = path[1][0] - path[0][0]
        dy = path[1][1] - path[0][1]
        for name, (ax, ay) in DIRS.items():
            if (ax, ay) == (dx, dy):
                return name
        return None

    def attack_target(self):
        """返回 (方向, 距离, 敌队) —— 直线 1–2 格内最近的、非保护敌车。"""
        best = None
        for team, info in self.enemies.items():
            if info["tick"] != self.last_tick:
                continue
            if info.get("hp", 0) <= 0:
                continue
            ex, ey = info["pos"]
            dx, dy = ex - self.pos[0], ey - self.pos[1]
            if dx != 0 and dy != 0:
                continue
            dist = abs(dx) + abs(dy)
            if not (1 <= dist <= 2):
                continue
            if info.get("protected"):
                continue
            direction = ("right" if dx > 0 else "left") if dy == 0 else ("down" if dy > 0 else "up")
            if best is None or dist < best[1]:
                best = (direction, dist, team)
        return best

    def escape_step(self, danger_radius=5):
        """风筝/撤退：选一个离敌车最远的合法邻格；全被堵则返回 None。"""
        enemies = [i["pos"] for i in self.enemies.values()]
        if not enemies:
            return None
        best = None
        for d, (dx, dy) in DIRS.items():
            nxt = (self.pos[0] + dx, self.pos[1] + dy)
            if not self.free(nxt):
                continue
            mind = min(self.manhattan(nxt, e) for e in enemies)
            score = (mind, 0.4 if d == self.last_move_dir else 0.0,
                     -nxt[0] * 0.001 - nxt[1] * 0.001)
            if best is None or score > best[0]:
                best = (score, d, mind)
        if best and best[2] < danger_radius:
            return best[1]
        return best[1] if best else None

    def choose_flee_target(self):
        """在 12 步可达范围内选离敌车最远、且未访问优先的落点。"""
        enemies = [i["pos"] for i in self.enemies.values()]
        if not enemies:
            return None
        # 从当前位置做一次有界 BFS，得到可达集合与步数
        dist = {self.pos: 0}
        q = deque([self.pos])
        while q:
            cur = q.popleft()
            if dist[cur] >= 12:
                continue
            for dx, dy in DIRS.values():
                nxt = (cur[0] + dx, cur[1] + dy)
                if nxt in dist or not self.free(nxt):
                    continue
                dist[nxt] = dist[cur] + 1
                q.append(nxt)
        best = None
        for cell, d in dist.items():
            if d == 0:
                continue
            mind = min(self.manhattan(cell, e) for e in enemies)
            score = (min(mind, 10), -d * 0.3, -0.6 if cell in self.visited else 0)
            if best is None or score > best[0]:
                best = (score, cell)
        return best[1] if best else None

    def explore_goal(self):
        """选一个尚未访问的格子；优先离当前位置近且离敌车远。"""
        candidates = []
        for y in range(self.map_h):
            for x in range(self.map_w):
                c = (x, y)
                if c in self.visited or c in self.obstacles:
                    continue
                if any(i["tick"] == self.last_tick and i["pos"] == c
                       for i in self.enemies.values()):
                    continue
                d = self.manhattan(self.pos, c)
                if d > 10:
                    continue
                danger = min((self.manhattan(c, i["pos"]) for i in self.enemies.values()),
                             default=9)
                candidates.append((danger, -d, c))
        if not candidates:
            return None
        candidates.sort(reverse=True)
        for _score, _d, cell in candidates[:3]:
            if self.bfs(self.pos, {cell}) is not None:
                return cell
        return None

    def go_to(self, goals, why, avoid_enemy_radius=0):
        path = self.bfs(self.pos, goals, avoid_enemy_radius=avoid_enemy_radius)
        if path and len(path) >= 2:
            return "move", self.first_dir(path), why
        # 目标不可达：贪心一步
        if goals:
            target = min(goals, key=lambda g: self.manhattan(self.pos, g))
            options = []
            for d, (dx, dy) in DIRS.items():
                nxt = (self.pos[0] + dx, self.pos[1] + dy)
                if self.free(nxt):
                    options.append((self.manhattan(nxt, target), d))
            if options:
                options.sort()
                return "move", options[0][1], why + "·贪心"
        return self.fallback_move(why)

    def fallback_move(self, why):
        options = []
        for d, (dx, dy) in DIRS.items():
            nxt = (self.pos[0] + dx, self.pos[1] + dy)
            if self.free(nxt):
                options.append((self.visited.__contains__(nxt), self.rng.random(), d))
        if options:
            options.sort()
            return "move", options[0][2], why + "·随机"
        return "defend", None, "无路可走转防御"

    # ---------------- 决策 ----------------
    def decide(self):
        if self.stuck >= 3:
            self.stuck = 0
            return self.fallback_move("卡住脱困")
        atk = self.attack_target()
        nearest = self.nearest_enemy()
        if self.profile == "dog":
            if atk and not self.protected:
                return "attack", atk[0], f"射程内开火（{atk[2]} 距{atk[1]}）"
            if nearest:
                return self.chase(nearest)
            return self.roam("疯狗找敌")
        if self.profile == "kite":
            if nearest and nearest["dist"] <= 6:
                if (self.flee_target is None or self.last_tick > self.flee_until
                        or nearest["dist"] <= 2):
                    self.flee_until = self.last_tick + 6
                    self.flee_target = self.choose_flee_target()
            if self.last_tick <= self.flee_until:
                if self.flee_target and self.flee_target != self.pos:
                    return self.go_to({self.flee_target},
                                      f"避战脱离（{nearest['team'] if nearest else '?'} 距"
                                      f"{nearest['dist'] if nearest else '?'}）")
                step = self.escape_step()
                if step:
                    return "move", step, "避战脱离·贴边"
            res = self.best_resource(max_dist=8, safe_from=5)
            if res:
                return self.go_to({res}, "安全窗口抢资源")
            return self.roam("风筝游走")
        # collector
        if nearest and nearest["dist"] <= 1 and self.hp <= 40:
            step = self.escape_step()
            if step:
                return "move", step, "残血近身脱离"
        if atk and not self.protected and (self.hp >= 50 or atk[1] == 1):
            return "attack", atk[0], f"机会还手（{atk[2]} 距{atk[1]}）"
        res = self.best_resource(max_dist=12, safe_from=0)
        if res:
            return self.go_to({res}, "采集资源")
        return self.roam("采集游走")

    def nearest_enemy(self):
        best = None
        for team, info in self.enemies.items():
            d = self.manhattan(self.pos, info["pos"])
            if best is None or d < best["dist"]:
                best = {"team": team, "pos": info["pos"], "dist": d, "hp": info.get("hp", 100)}
        return best

    def chase(self, nearest):
        """追猎：优先走到敌车当前格附近的攻击位（直线 1–2 格）。"""
        ex, ey = nearest["pos"]
        spots = [(ex + dx, ey + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))]
        spots += [(ex + 2 * dx, ey + 2 * dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))]
        spots = [c for c in spots if self.free(c, dynamic=False)
                 and (c[0] == ex or c[1] == ey)]
        if spots:
            return self.go_to(set(spots), f"直扑{nearest['team']}（距{nearest['dist']}）")
        return self.go_to({nearest["pos"]}, f"追{nearest['team']}贴近")

    def best_resource(self, max_dist, safe_from):
        best = None
        for rid, (x, y, v) in self.resources.items():
            c = (x, y)
            d = self.manhattan(self.pos, c)
            if d > max_dist:
                continue
            danger = min((self.manhattan(c, i["pos"]) for i in self.enemies.values()),
                         default=99)
            if danger <= safe_from:
                continue
            score = v * 2 - d * 3 + min(danger, 10)
            if best is None or score > best[0]:
                best = (score, c, rid)
        return best[1] if best else None

    def roam(self, why):
        goal = self.explore_goal()
        if goal:
            return self.go_to({goal}, why + "·探索")
        # 没有未探索格：绕中心巡逻
        center = (self.map_w // 2, self.map_h // 2)
        if self.manhattan(self.pos, center) > 1:
            return self.go_to({center}, why + "·回中心")
        return "wait", None, why + "·待机"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seat", required=True, choices=list("ABCD"))
    ap.add_argument("--profile", required=True, choices=["kite", "dog", "collector"])
    ap.add_argument("--name", default=None)
    ap.add_argument("--tick-ms", type=int, default=500)
    args = ap.parse_args()
    name = args.name or f"{args.profile}陪练{args.seat}"
    rclpy.init()
    node = Sparring(args.seat, args.profile, name, args.tick_ms)
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.get_logger().info(f"退出：共发动作 {node.actions_sent} 次，分布 {node.decisions}")
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
