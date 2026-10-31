// LLM 结论解析/校验自检（不依赖 ROS/网络，可独立运行）：
//   ros2 run rmtc_bot llm_conclusion_selftest
// 场景覆盖 docs/llm_prompt_design.md §1 schema 与 §4 本地校验规则；
// 退出码 0 = 全部通过，1 = 有失败。

#include <cstdio>
#include <string>
#include <vector>

#include "rmtc_bot/llm_conclusion.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* name) {
  ++g_checks;
  if (ok) {
    std::printf("[PASS] %s\n", name);
  } else {
    std::printf("[FAIL] %s\n", name);
    ++g_failures;
  }
}

// 设计稿 §1 的规范结论样例（rect [[8,3],[12,7]]）
const char* k_full =
    R"json({"goals":["resource_high","resource_normal","reach_target"],
 "event_plan":{"rect":[[8,3],[12,7]],"avoid":true,"until_tick":57,
               "reason":"幻术区域，移动颠倒"},
 "summary":"当前应优先采高价值资源，避开幻术区。"})json";

void test_full_conclusion() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(k_full, c, w), "完整结论: 解析成功");
  check(w.empty(), "完整结论: 无警告");
  check(c.goals.size() == 3 && c.goals[0] == "resource_high" &&
            c.goals[2] == "reach_target",
        "完整结论: goals 3 项保序");
  check(c.has_plan, "完整结论: has_plan=true");
  check(c.plan.avoid && c.plan.until_tick == 57, "完整结论: avoid/until_tick");
  check(c.plan.has_rect && c.plan.rect_x0 == 8 && c.plan.rect_y0 == 3 &&
            c.plan.rect_x1 == 12 && c.plan.rect_y1 == 7,
        "完整结论: rect 解析");
  check(c.summary.find("幻术") != std::string::npos, "完整结论: summary");
}

void test_rect_normalized() {
  // 端点反序写入也应归一化为 min/max
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  const char* j =
      R"json({"goals":[],"event_plan":{"rect":[[12,7],[8,3]],
               "until_tick":57},"summary":""})json";
  check(rmtc_bot::parse_conclusion(j, c, w), "rect 反序: 解析成功");
  check(c.plan.rect_x0 == 8 && c.plan.rect_y0 == 3 && c.plan.rect_x1 == 12 &&
            c.plan.rect_y1 == 7,
        "rect 反序: 归一化为 min/max");
}

void test_plan_null_and_missing() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":["a"],"event_plan":null,"summary":"s"})json", c, w),
        "plan null: 解析成功");
  check(!c.has_plan && w.empty(), "plan null: 无 plan 无警告");
  w.clear();
  check(rmtc_bot::parse_conclusion(R"json({"goals":[],"summary":""})json", c, w),
        "plan 缺失: 解析成功");
  check(!c.has_plan && w.empty(), "plan 缺失: 无 plan 无警告");
}

void test_malformed_top() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(!rmtc_bot::parse_conclusion("不是JSON", c, w), "顶层乱码: 返回 false");
  check(!w.empty(), "顶层乱码: 有警告");
  w.clear();
  check(!rmtc_bot::parse_conclusion("[1,2,3]", c, w), "顶层数组: 返回 false");
  check(!w.empty(), "顶层数组: 有警告");
}

void test_goals_filter_parse() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":["a",42,null,{"x":1},"b"],"summary":""})json", c, w),
        "goals 混类型: 解析成功");
  check(c.goals.size() == 2 && c.goals[0] == "a" && c.goals[1] == "b",
        "goals 混类型: 非字符串被忽略");
}

void test_until_tick_required() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"rect":[[0,0],[1,1]],
               "avoid":true},"summary":""})json",
            c, w),
        "缺 until_tick: 顶层解析成功");
  check(!c.has_plan && !w.empty(), "缺 until_tick: plan 作废 + 警告");
  w.clear();
  c = rmtc_bot::LlmConclusion{};
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"rect":[[0,0],[1,1]],
               "until_tick":"57"},"summary":""})json",
            c, w),
        "until_tick 非整数: 顶层解析成功");
  check(!c.has_plan && !w.empty(), "until_tick 非整数: plan 作废 + 警告");
}

void test_bad_rect() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"rect":[[0,0],[1,1],[2,2]],
               "until_tick":57},"summary":""})json",
            c, w),
        "坏 rect: 顶层解析成功");
  check(!c.has_plan && !w.empty(), "坏 rect: plan 作废 + 警告");
}

void test_points_form() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"points":[[1,2],[3,4]],
               "avoid":false,"until_tick":57},"summary":""})json",
            c, w),
        "points 形态: 解析成功");
  check(c.has_plan && !c.plan.has_rect && c.plan.points.size() == 2 &&
            c.plan.points[1][0] == 3 && c.plan.points[1][1] == 4,
        "points 形态: 多点解析");
  check(!c.plan.avoid, "points 形态: avoid=false 生效");
}

void test_unknown_keys() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":["a"],"event_plan":{"rect":[[0,0],[1,1]],
               "until_tick":57,"new_field":123},"extra":42,
               "nested":{"x":1},"summary":"s"})json",
            c, w),
        "未知键: 解析成功（前向兼容）");
  check(w.empty() && c.has_plan, "未知键: 无警告且 plan 有效");
}

void test_validate_clip_rect() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":["a"],"event_plan":{"rect":[[5,5],[30,30]],
               "until_tick":57},"summary":""})json",
            c, w),
        "越界 rect: 解析成功");
  check(rmtc_bot::validate_conclusion(c, 18, 18, 50, {"a"}, w),
        "越界 rect: 校验后仍有效");
  check(c.plan.rect_x0 == 5 && c.plan.rect_y0 == 5 && c.plan.rect_x1 == 17 &&
            c.plan.rect_y1 == 17,
        "越界 rect: 裁剪到 [0,18)×[0,18)");
}

void test_validate_rect_outside() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"rect":[[20,20],[30,30]],
               "until_tick":57},"summary":""})json",
            c, w),
        "全图外 rect: 解析成功");
  check(!rmtc_bot::validate_conclusion(c, 18, 18, 50, {}, w),
        "全图外 rect: 校验作废");
  check(!c.has_plan && !w.empty(), "全图外 rect: has_plan=false + 警告");
}

void test_validate_expired() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"rect":[[1,1],[3,3]],
               "until_tick":57},"summary":""})json",
            c, w),
        "过期结论: 解析成功");
  check(!rmtc_bot::validate_conclusion(c, 18, 18, 60, {}, w),
        "过期结论: until_tick<=当前 tick 作废");
  check(!c.has_plan && !w.empty(), "过期结论: has_plan=false + 警告");
}

void test_validate_goals_whitelist() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":["reach_target","hack","resource_high"],
               "summary":""})json",
            c, w),
        "白名单: 解析成功");
  const std::vector<std::string> allowed = {"resource_high", "reach_target"};
  rmtc_bot::validate_conclusion(c, 18, 18, 50, allowed, w);
  check(c.goals.size() == 2 && c.goals[0] == "reach_target" &&
            c.goals[1] == "resource_high",
        "白名单: 未知值过滤、顺序保留");

  // 白名单含任务 id 时，模型回 id 也保留（10/28 实测，问题解决记录 4.11）
  rmtc_bot::LlmConclusion c2;
  std::vector<std::string> w2;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":["collect_two_normal","reach"],
               "summary":""})json",
            c2, w2),
        "白名单(id): 解析成功");
  const std::vector<std::string> allowed2 = {"normal_resources", "reach",
                                             "collect_two_normal"};
  rmtc_bot::validate_conclusion(c2, 18, 18, 50, allowed2, w2);
  check(c2.goals.size() == 2 && c2.goals[0] == "collect_two_normal" &&
            c2.goals[1] == "reach",
        "白名单(id): id 与 type 混排均保留");
}

void test_validate_points_clip() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"points":[[1,1],[99,2]],
               "until_tick":57},"summary":""})json",
            c, w),
        "越界 points: 解析成功");
  check(rmtc_bot::validate_conclusion(c, 18, 18, 50, {}, w),
        "越界 points: 校验后仍有效");
  check(c.plan.points.size() == 1 && c.plan.points[0][0] == 1,
        "越界 points: 越界点被裁剪");
}

void test_validate_points_all_outside() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"points":[[99,99]],
               "until_tick":57},"summary":""})json",
            c, w),
        "全图外 points: 解析成功");
  check(!rmtc_bot::validate_conclusion(c, 18, 18, 50, {}, w),
        "全图外 points: 裁剪后空 → 作废");
  check(!c.has_plan, "全图外 points: has_plan=false");
}

void test_avoid_non_bool() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"rect":[[0,0],[1,1]],
               "avoid":"yes","until_tick":57},"summary":""})json",
            c, w),
        "avoid 非布尔: 解析成功");
  check(c.has_plan && c.plan.avoid, "avoid 非布尔: 按默认 true");
  check(!w.empty(), "avoid 非布尔: 有警告");
}

void test_summary_missing() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;
  check(rmtc_bot::parse_conclusion(R"json({"goals":[]})json", c, w),
        "summary 缺失: 解析成功");
  check(c.summary.empty() && w.empty(), "summary 缺失: 空串不崩无警告");
}

// 10/27 审查 M4 回归：超大整数不得静默按 32 位回绕穿透校验——
// 垃圾结论（LLM 幻觉）不能生成假规避区让策略整场逃离。
void test_huge_numbers_rejected() {
  rmtc_bot::LlmConclusion c;
  std::vector<std::string> w;

  // rect 坐标 2^32 回绕成 0 会变"合法矩形" → 必须作废
  w.clear();
  c = rmtc_bot::LlmConclusion{};
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"rect":[[4294967296,0],[1,1]],
               "until_tick":57},"summary":""})json",
            c, w),
        "超大 rect: 顶层解析成功");
  check(!c.has_plan && !w.empty(), "超大 rect: plan 作废 + 警告");

  // points 坐标越界 → 作废
  w.clear();
  c = rmtc_bot::LlmConclusion{};
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"points":[[4294967296,2]],
               "until_tick":57},"summary":""})json",
            c, w),
        "超大 points: 顶层解析成功");
  check(!c.has_plan && !w.empty(), "超大 points: plan 作废 + 警告");

  // until_tick=2^32 回绕成 0 → has_plan=false 却可能带假时效 → 必须作废
  w.clear();
  c = rmtc_bot::LlmConclusion{};
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"rect":[[0,0],[1,1]],
               "until_tick":4294967296},"summary":""})json",
            c, w),
        "超大 until_tick: 顶层解析成功");
  check(!c.has_plan && !w.empty(), "超大 until_tick: plan 作废 + 警告");

  // 负数 until_tick：解析成功但 has_plan=false（与既有约定一致）
  w.clear();
  c = rmtc_bot::LlmConclusion{};
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"rect":[[0,0],[1,1]],
               "until_tick":-5},"summary":""})json",
            c, w),
        "负数 until_tick: 顶层解析成功");
  check(!c.has_plan, "负数 until_tick: has_plan=false");

  // 浮点坐标（LLM 可能给小数）：有界读取截断取整，不产生 UB
  w.clear();
  c = rmtc_bot::LlmConclusion{};
  check(rmtc_bot::parse_conclusion(
            R"json({"goals":[],"event_plan":{"rect":[[1.9,2.9],[3.2,4.8]],
               "until_tick":57},"summary":""})json",
            c, w),
        "浮点 rect: 顶层解析成功");
  check(c.has_plan && c.plan.rect_x0 == 1 && c.plan.rect_y0 == 2 &&
            c.plan.rect_x1 == 3 && c.plan.rect_y1 == 4,
        "浮点 rect: 截断取整 [1,2]-[3,4]");
}

}  // namespace

int main() {
  test_full_conclusion();
  test_rect_normalized();
  test_plan_null_and_missing();
  test_malformed_top();
  test_goals_filter_parse();
  test_until_tick_required();
  test_bad_rect();
  test_points_form();
  test_unknown_keys();
  test_validate_clip_rect();
  test_validate_rect_outside();
  test_validate_expired();
  test_validate_goals_whitelist();
  test_validate_points_clip();
  test_validate_points_all_outside();
  test_avoid_non_bool();
  test_summary_missing();
  test_huge_numbers_rejected();

  if (g_failures == 0) {
    std::printf("\n全部通过（%d 项）。\n", g_checks);
    return 0;
  }
  std::printf("\n%d 项失败（共 %d 项）。\n", g_failures, g_checks);
  return 1;
}
