// LLM 结论解析与本地校验实现（M3 管道预备）。
// 详见 include/rmtc_bot/llm_conclusion.hpp 头注释与 docs/llm_prompt_design.md §1/§4。

#include "rmtc_bot/llm_conclusion.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace rmtc_bot {
namespace {

using nlohmann::json;

// 有界"数字→int"读取（10/27 审查 M4 修复）：LLM 结论里的坐标/时效先按
// double 取值做有限性与 int 范围检查，再截断取整——超大整数不再静默按
// 32 位回绕穿透校验生成假规避区；越界/非有限一律视为非法，作废对应字段。
bool bounded_number_int(const json& v, int& out) {
  if (!v.is_number()) return false;
  const double d = v.get<double>();
  if (!std::isfinite(d) || d < static_cast<double>(INT_MIN) ||
      d > static_cast<double>(INT_MAX)) {
    return false;
  }
  out = static_cast<int>(d);
  return true;
}

// 严格整数 + 有界读取（until_tick 等要求整数的字段用）
bool bounded_int(const json& v, int& out) {
  if (!v.is_number_integer()) return false;
  if (v.is_number_unsigned()) {
    const std::uint64_t uv = v.get<std::uint64_t>();
    if (uv > static_cast<std::uint64_t>(INT_MAX)) return false;
    out = static_cast<int>(uv);
    return true;
  }
  const std::int64_t sv = v.get<std::int64_t>();
  if (sv < INT_MIN || sv > INT_MAX) return false;
  out = static_cast<int>(sv);
  return true;
}

// 从 JSON 数组读 [[x,y],[x,y]] 形态的矩形；失败返回 false。
bool read_rect(const json& j, EventPlan& plan, std::vector<std::string>& warnings) {
  if (!j.is_array() || j.size() != 2 || !j[0].is_array() || !j[1].is_array()) {
    warnings.push_back("event_plan.rect 不是 [[x0,y0],[x1,y1]] 形态，忽略 rect");
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    if (j[i].size() != 2 || !j[i][0].is_number() || !j[i][1].is_number()) {
      warnings.push_back("event_plan.rect 端点不是两个数字，忽略 rect");
      return false;
    }
  }
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (!bounded_number_int(j[0][0], x0) || !bounded_number_int(j[0][1], y0) ||
      !bounded_number_int(j[1][0], x1) || !bounded_number_int(j[1][1], y1)) {
    warnings.push_back("event_plan.rect 坐标超出整数范围，忽略 rect");
    return false;
  }
  plan.rect_x0 = std::min(x0, x1);
  plan.rect_y0 = std::min(y0, y1);
  plan.rect_x1 = std::max(x0, x1);
  plan.rect_y1 = std::max(y0, y1);
  plan.has_rect = true;
  return true;
}

// 从 JSON 数组读 [[x,y], ...] 形态的多点集合；失败返回 false。
bool read_points(const json& j, EventPlan& plan, std::vector<std::string>& warnings) {
  if (!j.is_array() || j.empty()) {
    warnings.push_back("event_plan.points 不是非空数组，忽略 points");
    return false;
  }
  for (const auto& p : j) {
    if (!p.is_array() || p.size() != 2 || !p[0].is_number() ||
        !p[1].is_number()) {
      warnings.push_back("event_plan.points 含非 [x,y] 元素，忽略 points");
      return false;
    }
    int px = 0, py = 0;
    if (!bounded_number_int(p[0], px) || !bounded_number_int(p[1], py)) {
      warnings.push_back("event_plan.points 坐标超出整数范围，忽略 points");
      return false;
    }
    plan.points.push_back({px, py});
  }
  return true;
}

}  // namespace

bool parse_conclusion(const std::string& text, LlmConclusion& out,
                      std::vector<std::string>& warnings) {
  out = LlmConclusion{};
  json j;
  try {
    j = json::parse(text);
  } catch (const std::exception& e) {
    warnings.push_back(std::string("结论 JSON 解析失败：") + e.what());
    return false;
  }
  if (!j.is_object()) {
    warnings.push_back("结论顶层不是 JSON 对象");
    return false;
  }

  // goals：数组，非字符串元素忽略
  if (j.contains("goals")) {
    if (j["goals"].is_array()) {
      for (const auto& g : j["goals"]) {
        if (g.is_string()) out.goals.push_back(g.get<std::string>());
      }
    } else {
      warnings.push_back("goals 不是数组，忽略");
    }
  }

  // event_plan：null/缺失 → 无 plan；对象则逐字段宽松解析
  if (j.contains("event_plan") && j["event_plan"].is_object()) {
    const json& ep = j["event_plan"];
    bool plan_valid = true;

    if (ep.contains("avoid")) {
      if (ep["avoid"].is_boolean()) {
        out.plan.avoid = ep["avoid"].get<bool>();
      } else {
        warnings.push_back("event_plan.avoid 不是布尔，按默认 true");
      }
    }

    if (ep.contains("until_tick")) {
      if (bounded_int(ep["until_tick"], out.plan.until_tick)) {
        // 有界读取成功（M4：超 int 范围的整数直接作废，防回绕成"未来值"）
      } else {
        warnings.push_back("event_plan.until_tick 不是整数或超出范围");
        plan_valid = false;  // 时效是硬要求：缺失/非法 → 整个 plan 作废
      }
    } else {
      warnings.push_back("event_plan 缺少 until_tick（结论必须带时效），plan 作废");
      plan_valid = false;
    }

    if (ep.contains("reason") && ep["reason"].is_string()) {
      out.plan.reason = ep["reason"].get<std::string>();
    }

    // 区域表达：rect 优先；rect 坏则尝试 points；两者都缺 → plan 作废
    bool has_region = false;
    if (ep.contains("rect")) {
      has_region = read_rect(ep["rect"], out.plan, warnings);
    }
    if (!has_region && ep.contains("points")) {
      has_region = read_points(ep["points"], out.plan, warnings);
    }
    if (!has_region) {
      warnings.push_back("event_plan 无有效区域表达（rect/points），plan 作废");
      plan_valid = false;
    }

    if (plan_valid && out.plan.until_tick > 0) {
      out.has_plan = true;
    }
  } else if (j.contains("event_plan") && !j["event_plan"].is_null()) {
    warnings.push_back("event_plan 不是对象也不是 null，忽略");
  }

  // summary：字符串；缺失/非字符串 → 空串不崩
  if (j.contains("summary") && j["summary"].is_string()) {
    out.summary = j["summary"].get<std::string>();
  }

  return true;
}

bool validate_conclusion(LlmConclusion& out, int map_w, int map_h,
                         int current_tick,
                         const std::vector<std::string>& allowed_goals,
                         std::vector<std::string>& warnings) {
  // 1. goals 白名单过滤（§4.2）：未知值忽略
  std::vector<std::string> kept;
  for (const auto& g : out.goals) {
    if (allowed_goals.empty() ||
        std::find(allowed_goals.begin(), allowed_goals.end(), g) !=
            allowed_goals.end()) {
      kept.push_back(g);
    }
  }
  if (kept.size() != out.goals.size()) {
    warnings.push_back("goals 中有未知类型，已按白名单过滤");
  }
  out.goals = std::move(kept);

  // 2. 时效校验（§4.4）：过期作废
  if (out.has_plan && out.plan.until_tick <= current_tick) {
    warnings.push_back("event_plan 已过期（until_tick <= 当前 tick），作废");
    out.has_plan = false;
    return false;
  }
  if (!out.has_plan) return false;

  // 3. 坐标裁剪到 [0,W)×[0,H)（§4.3）；裁剪后空 → 作废
  if (out.plan.has_rect) {
    int x0 = std::max(0, out.plan.rect_x0);
    int y0 = std::max(0, out.plan.rect_y0);
    int x1 = std::min(map_w - 1, out.plan.rect_x1);
    int y1 = std::min(map_h - 1, out.plan.rect_y1);
    if (x0 > x1 || y0 > y1) {
      warnings.push_back("event_plan.rect 完全在地图外，作废");
      out.has_plan = false;
      return false;
    }
    out.plan.rect_x0 = x0;
    out.plan.rect_y0 = y0;
    out.plan.rect_x1 = x1;
    out.plan.rect_y1 = y1;
  }

  std::vector<std::array<int, 2>> kept_pts;
  for (const auto& p : out.plan.points) {
    if (p[0] >= 0 && p[0] < map_w && p[1] >= 0 && p[1] < map_h) {
      kept_pts.push_back(p);
    }
  }
  if (kept_pts.size() != out.plan.points.size()) {
    warnings.push_back("event_plan.points 有越界点，已裁剪");
  }
  out.plan.points = std::move(kept_pts);

  if (!out.plan.has_rect && out.plan.points.empty()) {
    warnings.push_back("event_plan 裁剪后无有效区域，作废");
    out.has_plan = false;
    return false;
  }
  return true;
}

}  // namespace rmtc_bot
