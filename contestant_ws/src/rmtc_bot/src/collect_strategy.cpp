// collect_strategy.cpp —— 第一阶段采集策略（10/25 实现，照 docs/collect_design.md）
//
// 设计底稿必读：docs/collect_design.md（§0 验收标准与可行性模拟、§2 记忆地图、
// §3 最近优先贪心、§4 BFS、§5 红线、§6.5 十三组 TDD 场景）。
//
// 实现结构（decide 一次调用的顺序）：
//   0. 同 Tick 幂等缓存（恢复重发同 Tick 输出逐字节一致）
//   1. 红线闸门：阵亡 → wait；无位置/地图 → wait；position_reason → 清 MOVING
//   2. 记忆更新：视野内障碍/资源入图；踩上且消失的资源删除；撞墙学障碍；卡住计数
//   3. 目标选择：无目标/目标失效 → 全图挑 BFS 最近可达资源；有目标 → 每帧重算
//      到目标的第一步；目标不可达 → 换目标；全不可达 → wait
//   4. 输出：只 move/wait，方向 ∈ up/down/left/right（阶段一白名单）
//
// 设计拍板记录（collect_design.md §7）：最近优先不加权（§0.1 模拟：满分 92%、
// 均 39.8）；卡住阈值 K=3；刚走过的格子不加惩罚；视野外资源保留在记忆。
// 10/21 线上实测这组决策 40/40 满分、invalid_actions=0（参考实现快照见
// docs/evidence/stage1_collect_refqa_*.json）。

#include <cstdint>
#include <cstdlib>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rmtc_bot/collect_strategy.hpp"

namespace rmtc_bot {
namespace {

using Cell = std::pair<int, int>;

// BFS 结果：{最短距离, 起点后第一步方向}；不可达时 dist=-1、first 为空
struct Route {
  int dist = -1;
  std::string first;
};

// 4 邻域 BFS（up/down/left/right，20×20 无脑写都行），每帧重算只取第一步。
// 回溯小坑（collect_design §4）：从终点沿 prev 走回，停在"prev==起点"的节点上，
// 别多退一格退到起点（方向会算空）。selftest 第 2/3/4/7/8/12 组具体方向断言守这道关。
// 越界护栏（10/27 审查 L19）：起点/终点/地图尺寸非法一律按不可达返回，
// 不产生越界访问。
Route BfsRoute(int sx, int sy, int tx, int ty, int w, int h,
               const std::set<Cell> &obstacles) {
  if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || tx < 0 || ty < 0 ||
      sx >= w || sy >= h || tx >= w || ty >= h) {
    return {-1, ""};
  }
  if (sx == tx && sy == ty) return {0, ""};
  std::vector<std::vector<int>> dist(h, std::vector<int>(w, -1));
  std::vector<std::vector<Cell>> prev(h, std::vector<Cell>(w, {-1, -1}));
  const int dx[4] = {0, 0, -1, 1};
  const int dy[4] = {-1, 1, 0, 0};
  std::queue<Cell> q;
  dist[sy][sx] = 0;
  q.push({sx, sy});
  while (!q.empty()) {
    Cell c = q.front();
    q.pop();
    if (c.first == tx && c.second == ty) break;
    for (int i = 0; i < 4; ++i) {
      int nx = c.first + dx[i];
      int ny = c.second + dy[i];
      if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      if (obstacles.count({nx, ny})) continue;
      if (dist[ny][nx] != -1) continue;
      dist[ny][nx] = dist[c.second][c.first] + 1;
      prev[ny][nx] = c;
      q.push({nx, ny});
    }
  }
  if (dist[ty][tx] < 0) return {-1, ""};
  // 从终点回溯到"起点之后的第一个节点"（其 prev 是起点）
  Cell step = {tx, ty};
  while (prev[step.second][step.first] != Cell(-1, -1) &&
         prev[step.second][step.first] != Cell(sx, sy)) {
    step = prev[step.second][step.first];
  }
  std::string dir;
  if (step.first == sx + 1 && step.second == sy) dir = "right";
  else if (step.first == sx - 1 && step.second == sy) dir = "left";
  else if (step.first == sx && step.second == sy + 1) dir = "down";
  else if (step.first == sx && step.second == sy - 1) dir = "up";
  return {dist[ty][tx], dir};
}

// 前沿探索（10/31 R6-A）：在所有已知可走格上 BFS，选一个落点，使「到达后 9×9
// 视野内新见格数 − 距离」最大。只用 seen_ 内的格当落点/路径 → 探索绝不撞墙。
// 为什么不是"最近的未访问格"：10/31 离线压测（docs/analysis/m2_obstacle_robust.py）
// 随机 20×20 80 张图，纯最近前沿 28.8 分、按新见格增益 37.4 分，差距来自前者
// 反复贴着已探索边界磨蹭、不往未见图推进。距离惩罚系数 1.0（在最终采用的已见格
// 版本上复测 λ=1.0→2.0：随机图差 ≤0.23 分、迷宫/DFS 完全不变，取整数运算的 1.0；
// 敏感性表见同脚本与 docs/analysis/stage1_exploration_20261031.md §3.2）。
Route ExploreRoute(int sx, int sy, int w, int h, int radius,
                   const std::set<Cell> &seen,
                   const std::set<Cell> &obstacles, int distance_weight = 1,
                   int center_x = -1, int center_y = -1,
                   int center_weight = 0) {
  if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || sx >= w || sy >= h) {
    return {-1, ""};
  }
  std::vector<std::vector<int>> dist(h, std::vector<int>(w, -1));
  std::vector<std::vector<Cell>> prev(h, std::vector<Cell>(w, {-1, -1}));
  const int dx[4] = {0, 0, -1, 1};
  const int dy[4] = {-1, 1, 0, 0};
  std::queue<Cell> q;
  dist[sy][sx] = 0;
  q.push({sx, sy});
  while (!q.empty()) {
    Cell c = q.front();
    q.pop();
    for (int i = 0; i < 4; ++i) {
      int nx = c.first + dx[i];
      int ny = c.second + dy[i];
      if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      Cell n{nx, ny};
      // 只走已见非障碍格：探索路径本身安全，不靠撞墙试错
      if (seen.count(n) == 0 || obstacles.count(n)) continue;
      if (dist[ny][nx] != -1) continue;
      dist[ny][nx] = dist[c.second][c.first] + 1;
      prev[ny][nx] = c;
      q.push(n);
    }
  }
  int best_score = INT32_MIN;
  int best_dist = INT32_MAX;
  Cell best{-1, -1};
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      Cell c{x, y};
      if (obstacles.count(c) || seen.count(c) == 0) continue;
      const int d = dist[y][x];
      if (d <= 0) continue;
      int gain = 0;
      for (int oy = -radius; oy <= radius; ++oy) {
        for (int ox = -radius; ox <= radius; ++ox) {
          const int nx = x + ox;
          const int ny = y + oy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          if (seen.count({nx, ny}) == 0) ++gain;
        }
      }
      if (gain == 0) continue;
      // 距离惩罚系数默认 1（整数运算，无浮点误差）；高价值搜索模式用更大
      // 系数（见调用点），优先扫完当前走廊/近处前沿，不急着横穿全图。
      // V8.8 开局冲中心：高价值搜索时给"更靠近地图中心"的落点加权，
      // 让无资源可追时也朝中心高价值区推进，而不是在出生侧磨蹭。
      // 坐标升序保证 tie-break 确定。
      int center_bonus = 0;
      if (center_weight > 0 && center_x >= 0 && center_y >= 0) {
        const int cur_center =
            std::abs(sx - center_x) + std::abs(sy - center_y);
        const int cell_center =
            std::abs(x - center_x) + std::abs(y - center_y);
        if (cell_center < cur_center) {
          center_bonus = center_weight * (cur_center - cell_center);
        }
      }
      const int score = gain - distance_weight * d + center_bonus;
      if (score > best_score ||
          (score == best_score &&
           (d < best_dist || (d == best_dist && c < best)))) {
        best_score = score;
        best_dist = d;
        best = c;
      }
    }
  }
  if (best.first < 0) return {-1, ""};
  Cell step = best;
  while (prev[step.second][step.first] != Cell(-1, -1) &&
         prev[step.second][step.first] != Cell(sx, sy)) {
    step = prev[step.second][step.first];
  }
  std::string dir;
  if (step.first == sx + 1 && step.second == sy) dir = "right";
  else if (step.first == sx - 1 && step.second == sy) dir = "left";
  else if (step.first == sx && step.second == sy + 1) dir = "down";
  else if (step.first == sx && step.second == sy - 1) dir = "up";
  return {best_dist, dir};
}

}  // namespace

ActionChoice CollectStrategy::decide(const Perception &p, const LlmResult *llm) {
  (void)llm;  // 阶段一无 LLM 决策需求

  // 0) 同帧幂等：恢复重发同 Tick 时输出逐字节一致（协议允许重复提交，selftest 10）
  if (has_cached_ && p.tick == last_tick_) return cached_;
  has_cached_ = true;
  last_tick_ = p.tick;
  auto emit = [this](ActionChoice c) {
    cached_ = c;
    return c;
  };

  // 1) 红线闸门（collect_design §5）
  if (p.self.has_alive && !p.self.alive) {
    return emit(ActionChoice::Wait("阵亡：等待复活"));
  }
  if (!p.self.has_position || p.map_width <= 0 || p.map_height <= 0) {
    return emit(ActionChoice::Wait("无有效位置或地图信息"));
  }
  // 重生/换位宣告：清 MOVING 状态重新选目标（障碍记忆保留，中幻术不怕）
  if (p.self.has_position_reason) {
    target_.clear();
    stuck_ = 0;
  }

  const int x = p.self.x;
  const int y = p.self.y;
  const int w = p.map_width;
  const int h = p.map_height;

  // 1b) 已见格：规则 §4.1 视野为横纵 ±vision_radius 的 9×9（障碍不挡视野，
  //     边缘不裁剪），所以每帧把窗口内所有地图内格子记为"已见"。这是探索增益
  //     的分母：只有"没见过的格子"才可能藏着没发现的资源。
  //     半径以感知帧的 vision_radius 为准（规则标准 4；缺失/非法按 4，和
  //     collect_selftest 的默认帧一致），不把地图尺寸/半径写死在策略里。
  const int vr = (p.vision_radius > 0 && p.vision_radius <= 4) ? p.vision_radius : 4;
  for (int oy = -vr; oy <= vr; ++oy) {
    for (int ox = -vr; ox <= vr; ++ox) {
      const int nx = x + ox;
      const int ny = y + oy;
      if (nx >= 0 && ny >= 0 && nx < w && ny < h) seen_.insert({nx, ny});
    }
  }

  // 2) 记忆更新（collect_design §2）
  // 2a) 本帧可见：障碍入集合、资源按 id upsert（每帧直接信任 self.position 绝对锚点）
  std::set<std::string> seen;
  std::set<Cell> frame_obs;  // 本帧视野内确实看到的障碍（阶段二动态阻挡判据）
  for (const auto &e : p.entities) {
    if (!e.has_absolute) continue;
    if (e.type == "obstacle") {
      obstacles_.insert({e.abs_x, e.abs_y});
      frame_obs.insert({e.abs_x, e.abs_y});
    } else if (e.type == "resource_normal" || e.type == "resource_high") {
      // L26 修复：协议资源均带 id；无 id 无法可靠记忆（坐标键会与真实 id
      // 冲突），直接跳过。
      if (e.id.empty()) continue;
      // V8.7：资源价值随记忆保存。线上正常帧一定带 value；缺失时按类型兜底
      // （resource_high=20 / resource_normal=10），不因字段缺失把高价值降级。
      const int value = (e.has_value && e.value > 0)
                            ? e.value
                            : (e.type == "resource_high" ? 20 : 10);
      res_[e.id] = Res{e.abs_x, e.abs_y, p.tick, value};
      seen.insert(e.id);
    }
  }
  // 2b) 删除记忆的条件（10/31 V8.6 补第二条）：
  //     ① 站在该资源格上且本帧看不到它（被我们采走）；
  //     ② 该格就在当前 9×9 视野内、却看不到资源（被别人抢走/已消失）。
  //        视野不挡、窗口内无实体 = 确定信息；再走过去只是白跑。15 场压测的
  //        迷宫局里 n03/h01/n06 三个资源都是"快到时被抢"，旧实现仍按记忆赶路，
  //        三个空趟约 20 Tick——足够再抢一个资源。
  //     视野外消失仍然保留（10/21 线上教训：按"得分上涨"批量删会丢全部视野外
  //     记忆，90/200 败北；已固化为 selftest 第 13 组）。
  for (auto it = res_.begin(); it != res_.end();) {
    bool gone = seen.count(it->first) == 0;
    const bool on_it = (it->second.x == x && it->second.y == y);
    const bool in_view = std::abs(it->second.x - x) <= vr &&
                         std::abs(it->second.y - y) <= vr;
    if (gone && (on_it || in_view)) {
      if (it->first == target_) target_.clear();
      it = res_.erase(it);
    } else {
      ++it;
    }
  }
  // 2c) 撞墙学障碍：上一动作 move 未动成且有方向（E_BLOCKED/E_OOB 或无错误码）
  //     阶段二（dynamic_blockers_）例外：敌车占位、幻术方向反转都会报 E_BLOCKED，
  //     把目标格学成永久的墙会污染整场地图（迷宫/多敌局实测 14–20 次，目标格
  //     逐帧核对全部可通行）。判据：本帧视野内该格没有障碍实体 → 不是墙；
  //     幻术生效期间有效方向与意图相反 → 连"目标格"都不是我们想走的那格。
  if (p.self.has_last_action && p.self.last_action == "move" &&
      !p.self.last_moved && p.self.has_last_direction) {
    bool learn = !p.self.has_last_error || p.self.last_error == "E_BLOCKED" ||
                 p.self.last_error == "E_OOB";
    if (learn && dynamic_blockers_) {
      if (illusion_) {
        learn = false;  // 有效方向被反转，学不到真实意图格
      } else {
        const std::string &d = p.self.last_effective_direction;
        int ox = x;
        int oy = y;
        if (d == "up") --oy;
        else if (d == "down") ++oy;
        else if (d == "left") --ox;
        else if (d == "right") ++ox;
        const bool in_window = std::abs(ox - x) <= vr && std::abs(oy - y) <= vr;
        if (in_window && frame_obs.count({ox, oy}) == 0) {
          learn = false;  // 视野内明明是空地 → 阻挡是动态的，不是墙
        }
      }
    }
    if (learn) {
      const std::string &d = p.self.last_effective_direction;
      // 方向值域校验（10/27 审查 L20）：非法方向一律不学习——否则 else 链
      // 会把"自己当前格"插入障碍集，永久污染地图。
      if (d == "up" || d == "down" || d == "left" || d == "right") {
        int ox = x;
        int oy = y;
        if (d == "up") --oy;
        else if (d == "down") ++oy;
        else if (d == "left") --ox;
        else if (d == "right") ++ox;
        if (ox >= 0 && oy >= 0 && ox < w && oy < h) obstacles_.insert({ox, oy});
      }
    }
  }
  // 2d) 卡住计数：只认 move 且未动成的回执（wait 回执的 moved=false 不算卡住）；
  //     连续 K=3 帧 → 放弃当前目标重选（collect_design §3/§7.2，线上 40/40 实测）。
  //     L21 修复：换目标必须排除刚放弃的目标，防止重选同一个目标陷入循环。
  if (p.self.has_last_action && p.self.last_action == "move" &&
      !p.self.last_moved) {
    ++stuck_;
    if (stuck_ >= 3) {
      excluded_target_ = target_;  // 本次重选跳过它
      target_.clear();
      stuck_ = 0;
    }
  } else {
    stuck_ = 0;
  }

  // 3) 目标选择
  //    无资源任务时：BFS 最近优先，价值不加权（stage1 既有行为，自检第 4 组）；
  //    阶段二有资源任务时：按任务类型给等效距离折扣，避免"近处普通资源截胡
  //    远处高价值资源"（V8.7，10/31 多墙/长廊复盘 P0）。
  //    exclude：重选时排除刚放弃的目标（L21），只生效一轮
  const auto mission_done = [&](const Mission &m) {
    for (const auto &id : p.score.mission_completed_ids)
      if (id == m.id) return true;
    return false;
  };
  bool normal_task_pending = false;
  bool high_task_pending = false;
  bool value_task_pending = false;
  for (const auto &m : p.missions) {
    if (mission_done(m)) continue;
    if (m.type == "normal_resources") normal_task_pending = true;
    else if (m.type == "high_resources") high_task_pending = true;
    else if (m.type == "resource_value") value_task_pending = true;
  }
  // 10/31 V8.6 资源争夺过滤（仅阶段二）：视野内敌车到该资源的 BFS 距离比我们
  // 更近（严格小于）→ 这趟大概率白跑，直接跳过。迷宫局实测 n03 被 C 先占
  // （tick 7 敌车就在资源格上，我们距离 5），旧逻辑仍走 5 Tick 空趟；同刻
  // h01 敌距 9、我距 4，改目标就能先拿到 20 分的高价值资源。
  // V8.7 高价值搜索模式：高价值任务未完成、记忆里还没有高价值资源、且地图
  // 还远未探完时，普通资源只顺手拿近处（≤8 步 BFS）。否则会像长廊局那样：
  // 开局只看见 13 步外的普通资源就一路追过去，把探出近处高价值资源/完成
  // high 任务的窗口让给对手。地图探到 75% 后恢复正常，避免全图无高价值时
  // 反而饿死普通资源线。
  bool any_high_known = false;
  for (const auto &kv : res_)
    if (kv.second.value >= 20) any_high_known = true;
  const double known_cov =
      (w > 0 && h > 0)
          ? static_cast<double>(seen_.size()) / (static_cast<double>(w) * h)
          : 0.0;
  const bool high_search =
      high_task_pending && !any_high_known && known_cov < 0.75;
  // V8.8 开局冲中心（10/31 现场反馈）：后期人人对抗分满，资源是决胜项。
  // 高价值任务未完成且还没看到高价值资源时，普通资源按"是否更靠近中心"
  // 加权——沿着普通资源线往中心高价值区推，而不是被出生点附近的小资源
  // 反复截胡。只在高价值搜索窗口内生效；一旦看到高价值资源即恢复原优先级。
  const int center_x = (w - 1) / 2;
  const int center_y = (h - 1) / 2;
  const int our_center_dist =
      std::abs(x - center_x) + std::abs(y - center_y);

  const auto enemy_contests = [&](int rx, int ry, int our_d) {
    for (const auto &e : p.entities) {
      if (e.type != "robot" || e.team == p.self.team || !e.has_absolute)
        continue;
      Route er = BfsRoute(e.abs_x, e.abs_y, rx, ry, w, h, obstacles_);
      if (er.dist >= 0 && er.dist < our_d) return true;
    }
    return false;
  };
  auto pick_target = [&](const std::string &exclude) -> Route {
    std::string best;
    int best_rank = INT32_MAX;
    int best_dist = INT32_MAX;
    int best_value = -1;
    Route best_route;
    for (const auto &kv : res_) {
      if (!exclude.empty() && kv.first == exclude) continue;
      if (kv.second.x == x && kv.second.y == y) continue;  // 正踩着的格子下一帧结算
      Route rt = BfsRoute(x, y, kv.second.x, kv.second.y, w, h, obstacles_);
      if (rt.dist < 0) continue;  // 不可达（被墙围死）跳过
      if (dynamic_blockers_ && enemy_contests(kv.second.x, kv.second.y, rt.dist))
        continue;  // 阶段二：对手更近，不送空趟
      const bool high_value = kv.second.value >= 20;
      // V8.8：远处普通资源只要在"往中心走"的方向上就允许顺路拿；背向中心的
      // 远处普通资源仍然跳过，避免把开局窗口浪费在出生侧。
      const int res_center_dist =
          std::abs(kv.second.x - center_x) + std::abs(kv.second.y - center_y);
      const int center_gain = our_center_dist - res_center_dist;
      if (high_search && !high_value && rt.dist > 8 && center_gain <= 0) continue;
      int bonus = 0;
      // 高价值任务未完成：高价值资源最多可比普通资源多绕 10 步（报告建议
      // 8–12 步）；普通任务未完成：普通资源最多多绕 8 步。两个任务都在时
      // 高价值优先（资源分 20 vs 10，且任务同等 5 分）。
      if (high_task_pending && high_value) bonus += 10;
      if (normal_task_pending && !high_value) bonus += 8;
      if (value_task_pending) bonus += high_value ? 4 : 0;
      // V8.8：每靠近中心 1 格，等效抵扣 2 步绕行；普通资源因此会沿中心
      // 方向排队，但不会为了中心放弃脚下的资源。
      const int center_bonus =
          (high_search && center_gain > 0) ? 2 * center_gain : 0;
      // 折扣后距离小者优先；同 rank 取实际路径短者，再取价值高者，最后 id
      // （res_ 是 std::map，id 有序，保证 tie-break 确定）。
      const int rank = rt.dist - bonus - center_bonus;
      if (rank < best_rank ||
          (rank == best_rank &&
           (rt.dist < best_dist ||
            (rt.dist == best_dist &&
             (kv.second.value > best_value ||
              (kv.second.value == best_value && kv.first < best)))))) {
        best = kv.first;
        best_rank = rank;
        best_dist = rt.dist;
        best_value = kv.second.value;
        best_route = rt;
      }
    }
    if (best.empty()) return {-1, ""};
    target_ = best;
    return best_route;
  };

  const auto target_note = [&]() -> std::string {
    if (target_.empty() || res_.count(target_) == 0) return "";
    const bool high_value = res_[target_].value >= 20;
    if (high_task_pending && high_value) return "（高价值任务优先）";
    if (normal_task_pending && !high_value) return "（普通资源任务优先）";
    if (high_search) return "（向中心搜索高价值）";
    return "";
  };

  // 探索兜底（R6-A）：已知资源全不可达/记忆为空时，不再原地 wait。
  // 选择"到达后新见格最多 − 距离"的已见格，只走已见格，绝不撞墙。
  auto explore_or_wait = [&](const char *why) -> ActionChoice {
    // 阶段二组合复用（explore_enabled_=false）：保持旧行为，没资源就 wait，
    // 把决定权交回战斗层的环形巡逻/追猎。阶段一才走探索。
    if (!explore_enabled_) return emit(ActionChoice::Wait(why));
    // 高价值任务未完成且没有高价值记忆：近处优先（λ=6），先扫当前走廊，
    // 避免为了远处大块未见图一路横穿、把近处资源让给对手（长廊局实测）。
    // V8.8：同时给"更靠中心"的已见落点加权，无资源可追时也向中心推进。
    Route ex = ExploreRoute(x, y, w, h, vr, seen_, obstacles_,
                            high_search ? 6 : 1, center_x, center_y,
                            high_search ? 2 : 0);
    if (ex.dist < 0) {
      return emit(ActionChoice::Wait(std::string(why) + "，且已无可探索区域"));
    }
    return emit(ActionChoice::Move(
        ex.first, std::string(why) + "，探索未见图" +
                      (high_search ? "（向中心搜索高价值）" : "") + "（已见 " +
                      std::to_string(seen_.size()) + " 格，目标距 " +
                      std::to_string(ex.dist) + "）"));
  };

  if (target_.empty() || res_.count(target_) == 0) {
    target_.clear();
    Route rt = pick_target(excluded_target_);
    excluded_target_.clear();  // 排除只生效一轮，避免资源恢复可达后仍被永久跳过
    if (rt.dist < 0) return explore_or_wait("无可达资源");
    return emit(ActionChoice::Move(
        rt.first, "朝资源 " + target_ + " 移动（距 " + std::to_string(rt.dist) +
                      "，记忆 " + std::to_string(res_.size()) + " 资源）" +
                      target_note()));
  }
  // 已有目标：每帧重算第一步（自动适应撞墙/资源消失/换位）
  Route rt = BfsRoute(x, y, res_[target_].x, res_[target_].y, w, h, obstacles_);
  if (rt.dist == 0) {
    // L18 修复：正站在目标格上（资源仍可见、本帧结算）→ 直接 wait，
    // 不发方向为空的 Move("") 浪费 1 Tick（此前靠 bot_node 兜底降级）。
    // 注意：结算后资源消失，下一帧会删记忆并重新选目标/探索。
    return emit(ActionChoice::Wait("站在目标资源格上，等待结算"));
  }
  if (rt.dist < 0) {
    target_.clear();
    Route rt2 = pick_target(excluded_target_);
    excluded_target_.clear();
    if (rt2.dist < 0) return explore_or_wait("目标不可达且无备选");
    return emit(ActionChoice::Move(
        rt2.first, "朝资源 " + target_ + " 移动（距 " + std::to_string(rt2.dist) +
                       "，记忆 " + std::to_string(res_.size()) + " 资源）" +
                       target_note()));
  }
  return emit(ActionChoice::Move(
      rt.first, "朝资源 " + target_ + " 移动（距 " + std::to_string(rt.dist) +
                    "，记忆 " + std::to_string(res_.size()) + " 资源）" +
                    target_note()));
}

double CollectStrategy::explored_fraction(int w, int h) const {
  if (w <= 0 || h <= 0) return 0.0;
  const double total = static_cast<double>(w) * static_cast<double>(h);
  const double seen = static_cast<double>(seen_.size());
  if (seen >= total) return 1.0;
  return seen / total;
}

void CollectStrategy::on_new_session(const std::string &session_id) {
  (void)session_id;
  res_.clear();
  obstacles_.clear();
  seen_.clear();
  target_.clear();
  excluded_target_.clear();
  stuck_ = 0;
  last_tick_ = -2;
  has_cached_ = false;
  cached_ = ActionChoice{};
}

}  // namespace rmtc_bot
