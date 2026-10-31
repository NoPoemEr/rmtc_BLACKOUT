// combat_llm_guide.cpp —— LLM 意图翻译层实现（纯函数，见头文件注释）
#include "rmtc_bot/combat_llm_guide.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <map>
#include <set>

namespace rmtc_bot {

namespace {

// 方向枚举顺序（同分时靠前优先；与协议方向字符串一一对应）
const char *const kDirs[4] = {"up", "right", "down", "left"};
const int kDx[4] = {0, 1, 0, -1};
const int kDy[4] = {-1, 0, 1, 0};

bool tile_in_map(int x, int y, int w, int h) {
  return x >= 0 && y >= 0 && x < w && y < h;
}

// 从 (x,y) 走到回避区外所需的最少步数（直线计）；区外为 0。
// 仅在 plan 为有效 avoid 区时有意义；调用前已过滤。
int exit_distance(const EventPlan &plan, int x, int y) {
  if (!plan_blocks_tile(plan, x, y)) return 0;
  if (plan.has_rect) {
    int dx;
    if (x < plan.rect_x0 || x > plan.rect_x1) {
      dx = 0;
    } else if (plan.rect_x0 == plan.rect_x1) {
      dx = 1;  // 单列矩形：往左或往右一步即出
    } else {
      dx = std::min(x - plan.rect_x0, plan.rect_x1 - x) + 1;
    }
    int dy;
    if (y < plan.rect_y0 || y > plan.rect_y1) {
      dy = 0;
    } else if (plan.rect_y0 == plan.rect_y1) {
      dy = 1;  // 单行矩形：往上或往下一步即出
    } else {
      dy = std::min(y - plan.rect_y0, plan.rect_y1 - y) + 1;
    }
    return std::min(dx, dy);
  }
  // 点位集合：站上点 → 1 步；相邻及以外 → 0
  int best = INT_MAX;
  for (const auto &pt : plan.points) {
    best = std::min(best, std::abs(x - pt[0]) + std::abs(y - pt[1]));
  }
  return (best <= 0) ? 1 : 0;
}

}  // namespace

std::vector<std::string> mission_type_list(const std::vector<Mission> &missions) {
  std::vector<std::string> types;
  for (const auto &m : missions) {
    if (m.type.empty()) continue;
    if (std::find(types.begin(), types.end(), m.type) == types.end()) {
      types.push_back(m.type);
    }
  }
  return types;
}

bool prepare_conclusion(const LlmResult &result, const Perception &p,
                        LlmConclusion &out, std::vector<std::string> &warnings) {
  if (result.status != LlmResult::Status::SUCCESS) return false;
  if (!parse_conclusion(result.content, out, warnings)) return false;
  validate_conclusion(out, p.map_width, p.map_height, p.tick,
                      mission_type_list(p.missions), warnings);
  return conclusion_usable(out);
}

std::vector<const Mission *> missions_by_goal_priority(
    const std::vector<std::string> &goals, const std::vector<Mission> &missions,
    const std::vector<std::string> &completed_ids) {
  // goals → 首次出现序号；重复值只记第一次（稳定性由 stable_sort 保证）
  std::map<std::string, int> rank;
  int next_rank = 0;
  for (const auto &g : goals) {
    if (!g.empty() && rank.find(g) == rank.end()) {
      rank[g] = next_rank++;
    }
  }
  std::set<std::string> done(completed_ids.begin(), completed_ids.end());

  // 候选：未完成、可导航（reach 必须带坐标）
  std::vector<std::pair<int, const Mission *>> cand;
  for (const auto &m : missions) {
    if (done.count(m.id)) continue;
    if (m.type == "reach" && !m.has_position) continue;
    auto it = rank.find(m.type);
    // 模型可能回任务 id（10/28 实测，见问题解决记录 4.11）：id 同样参与排序
    if (it == rank.end()) it = rank.find(m.id);
    cand.emplace_back(it == rank.end() ? INT_MAX : it->second, &m);
  }
  std::stable_sort(cand.begin(), cand.end(),
                   [](const std::pair<int, const Mission *> &a,
                      const std::pair<int, const Mission *> &b) {
                     return a.first < b.first;
                   });

  std::vector<const Mission *> out;
  out.reserve(cand.size());
  for (const auto &c : cand) out.push_back(c.second);
  return out;
}

bool plan_active(const LlmConclusion &c, int tick) {
  if (!c.has_plan) return false;
  if (!c.plan.has_rect && c.plan.points.empty()) return false;
  if (c.plan.until_tick <= 0) return false;
  return c.plan.until_tick > tick;
}

bool plan_blocks_tile(const EventPlan &plan, int x, int y) {
  if (!plan.avoid) return false;
  if (plan.has_rect) {
    return x >= plan.rect_x0 && x <= plan.rect_x1 && y >= plan.rect_y0 &&
           y <= plan.rect_y1;
  }
  for (const auto &pt : plan.points) {
    if (x == pt[0] && y == pt[1]) return true;
  }
  return false;
}

ActionChoice redirect_avoid_plan(
    const EventPlan &plan, int cur_x, int cur_y, int map_w, int map_h,
    const std::set<std::pair<int, int>> &blocked_tiles,
    const ActionChoice &candidate) {
  // 只修正 move；且必须有生效的 avoid 区域表达
  if (candidate.action != "move") return candidate;
  if (!plan.avoid || (!plan.has_rect && plan.points.empty())) return candidate;

  // 候选目的格；未知方向按"原地"处理（= 当前位置）
  int dest_x = cur_x;
  int dest_y = cur_y;
  for (int i = 0; i < 4; ++i) {
    if (candidate.direction == kDirs[i]) {
      dest_x = cur_x + kDx[i];
      dest_y = cur_y + kDy[i];
      break;
    }
  }
  if (!plan_blocks_tile(plan, dest_x, dest_y)) return candidate;

  const int cur_score = exit_distance(plan, cur_x, cur_y);  // 0 = 区外
  int best_i = -1;
  int best_score = INT_MAX;
  for (int i = 0; i < 4; ++i) {
    const int nx = cur_x + kDx[i];
    const int ny = cur_y + kDy[i];
    if (!tile_in_map(nx, ny, map_w, map_h)) continue;
    if (blocked_tiles.count({nx, ny})) continue;
    const int s = exit_distance(plan, nx, ny);
    if (cur_score == 0 && s > 0) continue;  // 区外：不主动踩进回避区
    if (s < best_score) {
      best_score = s;
      best_i = i;
    }
  }
  if (best_i < 0) {
    return ActionChoice::Wait("llm-avoid: 回避区无安全方向，原地等待");
  }
  return ActionChoice::Move(kDirs[best_i], "llm-avoid: 避开事件回避区");
}

}  // namespace rmtc_bot
