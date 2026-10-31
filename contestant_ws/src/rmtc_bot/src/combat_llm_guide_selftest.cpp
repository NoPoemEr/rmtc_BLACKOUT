// combat_llm_guide_selftest.cpp —— LLM 意图翻译层离线自检
// （不依赖 ROS/裁判/网络；纯函数直接调用）
//
// 覆盖 5 组：prepare_conclusion 单入口 / 目标优先级排序 / plan 时效与区域判定 /
// 回避重定向（含障碍、区外不进入、单列矩形、点位区）/ 幂等回退。
// 场景清单与设计依据：docs/combat_design.md §2 第 3 层 + llm_conclusion.hpp §4。

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "rmtc_bot/combat_llm_guide.hpp"

using rmtc_bot::ActionChoice;
using rmtc_bot::EventPlan;
using rmtc_bot::LlmConclusion;
using rmtc_bot::LlmResult;
using rmtc_bot::Mission;
using rmtc_bot::Perception;

namespace {

int g_fail = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
      ++g_fail;                                                            \
    }                                                                      \
    std::fflush(stdout);                                                   \
  } while (0)

// ---- 构造辅助 ----

LlmResult Ok(const std::string &content) {
  LlmResult r;
  r.status = LlmResult::Status::SUCCESS;
  r.content = content;
  return r;
}

LlmResult Failed(LlmResult::Status s = LlmResult::Status::NETWORK_ERROR) {
  LlmResult r;
  r.status = s;
  return r;
}

Mission Ms(const std::string &id, const std::string &type, bool has_pos = false,
           int px = 0, int py = 0) {
  Mission m;
  m.id = id;
  m.type = type;
  m.has_position = has_pos;
  m.pos_x = px;
  m.pos_y = py;
  return m;
}

Perception Base(int w = 18, int h = 18, int tick = 100) {
  Perception p;
  p.map_width = w;
  p.map_height = h;
  p.tick = tick;
  return p;
}

// ---- 1. prepare_conclusion 单入口 ----

void TestPrepare() {
  std::printf("== prepare_conclusion（解析+校验单入口）==\n");
  std::vector<std::string> warns;

  {  // 1. 正常：goals 过白名单、rect 归一化
    Perception p = Base();
    p.missions = {Ms("m1", "reach", true, 3, 4), Ms("m2", "high_resources")};
    LlmConclusion c;
    bool ok = rmtc_bot::prepare_conclusion(
        Ok(R"({"goals":["reach","high_resources","junk"],"event_plan":
              {"rect":[[5,6],[2,3]],"avoid":true,"until_tick":120,"reason":"r"},
              "summary":"s"})"),
        p, c, warns);
    CHECK(ok);
    CHECK(c.goals.size() == 2 && c.goals[0] == "reach" && c.goals[1] == "high_resources");
    CHECK(c.has_plan);
    CHECK(c.plan.rect_x0 == 2 && c.plan.rect_y0 == 3 && c.plan.rect_x1 == 5 &&
          c.plan.rect_y1 == 6);
    CHECK(c.plan.until_tick == 120 && c.plan.avoid);
  }
  {  // 2. 非成功状态 → false，不回退读 out
    Perception p = Base();
    LlmConclusion c;
    bool ok = rmtc_bot::prepare_conclusion(Failed(), p, c, warns);
    CHECK(!ok);
  }
  {  // 3. 顶层非 JSON → false 且告警非空
    Perception p = Base();
    LlmConclusion c;
    warns.clear();
    bool ok = rmtc_bot::prepare_conclusion(Ok("not json at all"), p, c, warns);
    CHECK(!ok);
    CHECK(!warns.empty());
  }
  {  // 4. until_tick 已到期 → plan 作废，goals 仍可用
    Perception p = Base(18, 18, 100);
    p.missions = {Ms("m1", "reach", true, 1, 1)};
    LlmConclusion c;
    bool ok = rmtc_bot::prepare_conclusion(
        Ok(R"({"goals":["reach"],"event_plan":{"rect":[[0,0],[5,5]],
              "avoid":true,"until_tick":100},"summary":"s"})"),
        p, c, warns);
    CHECK(ok);  // goals 仍在 → 可用
    CHECK(!c.has_plan);
    CHECK(rmtc_bot::plan_active(c, 100) == false);
  }
  {  // 5. goals 全部不在任务类型里 + 无 plan → false（不可用，规则层兜底）
    Perception p = Base();
    p.missions = {Ms("m1", "reach", true, 1, 1)};
    LlmConclusion c;
    bool ok = rmtc_bot::prepare_conclusion(
        Ok(R"({"goals":["resource_high","resource_normal"],"event_plan":null,
              "summary":"s"})"),
        p, c, warns);
    CHECK(!ok);
    CHECK(c.goals.empty() && !c.has_plan);
  }
  {  // 6. rect 越界 → 裁剪进地图
    Perception p = Base(18, 18, 100);
    p.missions = {Ms("m1", "reach", true, 1, 1)};
    LlmConclusion c;
    bool ok = rmtc_bot::prepare_conclusion(
        Ok(R"({"goals":["reach"],"event_plan":{"rect":[[-5,-5],[50,50]],
              "avoid":true,"until_tick":120},"summary":"s"})"),
        p, c, warns);
    CHECK(ok);
    CHECK(c.has_plan && c.plan.rect_x0 == 0 && c.plan.rect_y0 == 0 &&
          c.plan.rect_x1 == 17 && c.plan.rect_y1 == 17);
  }
  {  // 7. points 表达 + avoid=false（goto 建议）同样合法
    Perception p = Base(18, 18, 100);
    p.missions = {Ms("m1", "reach", true, 1, 1)};
    LlmConclusion c;
    bool ok = rmtc_bot::prepare_conclusion(
        Ok(R"({"goals":["reach"],"event_plan":{"points":[[2,2],[4,4]],
              "avoid":false,"until_tick":120},"summary":"s"})"),
        p, c, warns);
    CHECK(ok);
    CHECK(c.has_plan && !c.plan.has_rect && c.plan.points.size() == 2);
    CHECK(!c.plan.avoid);
  }
}

// ---- 2. missions_by_goal_priority ----

void TestPriority() {
  std::printf("== missions_by_goal_priority（目标优先级排序）==\n");
  std::vector<Mission> ms = {
      Ms("a", "normal_resources"),  // 原序 0
      Ms("b", "high_resources"),    // 原序 1
      Ms("c", "effective_damage"),  // 原序 2
      Ms("d", "reach", true, 7, 8),       // 原序 3
      Ms("e", "reach", false, 0, 0),      // 原序 4：无坐标 → 跳过
      Ms("f", "normal_resources"),  // 原序 5
  };

  {  // 8. goals 顺序即优先级；未覆盖的按原序追加
    auto out = rmtc_bot::missions_by_goal_priority(
        {"effective_damage", "normal_resources"}, ms, {});
    CHECK(out.size() == 5);
    CHECK(out[0]->id == "c");
    CHECK(out[1]->id == "a");
    CHECK(out[2]->id == "f");
    CHECK(out[3]->id == "b");  // high_resources 未被覆盖 → 追加
    CHECK(out[4]->id == "d");  // reach（有坐标）未被覆盖 → 追加
  }
  {  // 9. 未知 goal 忽略；10. 已完成 id 跳过；11. 无坐标 reach 跳过
    auto out = rmtc_bot::missions_by_goal_priority(
        {"zzz", "reach"}, ms, {"a", "f"});
    CHECK(out.size() == 3);
    CHECK(out[0]->id == "d");  // reach 有坐标
    CHECK(out[1]->id == "b");  // 未覆盖追加（a/f 已完成跳过，e 无坐标跳过）
    CHECK(out[2]->id == "c");
  }
  {  // 12. goals 重复 → 首次出现定序；全部完成 → 空
    auto out = rmtc_bot::missions_by_goal_priority(
        {"normal_resources", "reach", "normal_resources"}, ms, {});
    CHECK(out.size() == 5);
    CHECK(out[0]->id == "a");
    CHECK(out[1]->id == "f");
    CHECK(out[2]->id == "d");
    CHECK(out[3]->id == "b");
    CHECK(out[4]->id == "c");
    auto none = rmtc_bot::missions_by_goal_priority(
        {"normal_resources"}, ms, {"a", "b", "c", "d", "e", "f"});
    CHECK(none.empty());
  }
  {  // 13. 空 goals → 全部按原序（未完成的）
    auto out = rmtc_bot::missions_by_goal_priority({}, ms, {"a"});
    CHECK(out.size() == 4);
    CHECK(out[0]->id == "b" && out[1]->id == "c" && out[2]->id == "d" &&
          out[3]->id == "f");
  }
  {  // 14. 模型回任务 id（10/28 实测，4.11）：id 也参与排序
    auto out = rmtc_bot::missions_by_goal_priority(
        {"b", "d"}, ms, {});
    CHECK(out.size() == 5);
    CHECK(out[0]->id == "b");
    CHECK(out[1]->id == "d");
    CHECK(out[2]->id == "a");  // 其余按原序追加
    CHECK(out[3]->id == "c");
    CHECK(out[4]->id == "f");
  }
}

// ---- 3. plan_active / plan_blocks_tile ----

EventPlan RectPlan(int x0, int y0, int x1, int y1, int until, bool avoid = true) {
  EventPlan p;
  p.avoid = avoid;
  p.has_rect = true;
  p.rect_x0 = x0;
  p.rect_y0 = y0;
  p.rect_x1 = x1;
  p.rect_y1 = y1;
  p.until_tick = until;
  return p;
}

void TestPlan() {
  std::printf("== plan_active / plan_blocks_tile（时效与区域判定）==\n");

  LlmConclusion c;
  c.has_plan = true;
  c.plan = RectPlan(2, 3, 5, 6, 120);
  CHECK(rmtc_bot::plan_active(c, 119));
  CHECK(!rmtc_bot::plan_active(c, 120));  // 到期当帧即失效
  c.plan.until_tick = -1;                 // 未设置 → 无效
  CHECK(!rmtc_bot::plan_active(c, 0));
  c.plan.until_tick = 120;
  c.has_plan = false;
  CHECK(!rmtc_bot::plan_active(c, 100));

  const EventPlan &rp = c.plan;
  CHECK(rmtc_bot::plan_blocks_tile(rp, 2, 3));   // 左上角（含端点）
  CHECK(rmtc_bot::plan_blocks_tile(rp, 5, 6));   // 右下角（含端点）
  CHECK(rmtc_bot::plan_blocks_tile(rp, 3, 5));   // 内部
  CHECK(!rmtc_bot::plan_blocks_tile(rp, 1, 3));  // 左外
  CHECK(!rmtc_bot::plan_blocks_tile(rp, 3, 7));  // 下外

  EventPlan gp = RectPlan(2, 3, 5, 6, 120, /*avoid=*/false);
  CHECK(!rmtc_bot::plan_blocks_tile(gp, 3, 4));  // goto 建议不拦路

  EventPlan pp;
  pp.avoid = true;
  pp.points = {{2, 2}, {8, 8}};
  CHECK(rmtc_bot::plan_blocks_tile(pp, 2, 2));
  CHECK(rmtc_bot::plan_blocks_tile(pp, 8, 8));
  CHECK(!rmtc_bot::plan_blocks_tile(pp, 3, 2));

  EventPlan empty_plan;  // 无区域表达 → 不拦任何格
  CHECK(!rmtc_bot::plan_blocks_tile(empty_plan, 0, 0));
}

// ---- 4. redirect_avoid_plan ----

void TestRedirect() {
  std::printf("== redirect_avoid_plan（回避重定向）==\n");
  const std::set<std::pair<int, int>> no_block;

  {  // 14. 非 move（attack）→ 原样
    ActionChoice c = ActionChoice::Attack("right", "敌车");
    auto out = rmtc_bot::redirect_avoid_plan(RectPlan(0, 0, 5, 5, 200), 1, 1,
                                             18, 18, no_block, c);
    CHECK(out.action == "attack" && out.direction == "right");
  }
  {  // 15. 目的格不在回避区 → 原样
    ActionChoice c = ActionChoice::Move("left", "去采集");
    auto out = rmtc_bot::redirect_avoid_plan(RectPlan(5, 5, 6, 6, 200), 1, 1,
                                             18, 18, no_block, c);
    CHECK(out.action == "move" && out.direction == "left");
  }
  {  // 16. 区外、候选会踩进去、有安全方向 → 换成安全方向（up 靠前）
    ActionChoice c = ActionChoice::Move("right", "去采集");  // (1,2) 在区内
    auto out = rmtc_bot::redirect_avoid_plan(RectPlan(1, 1, 3, 3, 200), 0, 2,
                                             18, 18, no_block, c);
    CHECK(out.action == "move");
    CHECK(out.direction == "up");  // (0,1) 区外，up 在枚举里最靠前
  }
  {  // 17. 区外、安全方向全被障碍占 → wait
    ActionChoice c = ActionChoice::Move("right", "去采集");
    const std::set<std::pair<int, int>> blocked = {{0, 1}, {0, 3}};
    auto out = rmtc_bot::redirect_avoid_plan(RectPlan(1, 1, 3, 3, 200), 0, 2,
                                             18, 18, blocked, c);
    CHECK(out.action == "wait");
  }
  {  // 18. 区内靠左 → 左一步即出
    ActionChoice c = ActionChoice::Move("right", "采集");  // (2,2) 仍在区内
    auto out = rmtc_bot::redirect_avoid_plan(RectPlan(1, 1, 3, 3, 200), 1, 2,
                                             18, 18, no_block, c);
    CHECK(out.action == "move" && out.direction == "left");
  }
  {  // 19. 区内、出口被障碍占 → 次优方向
    ActionChoice c = ActionChoice::Move("right", "采集");
    const std::set<std::pair<int, int>> blocked = {{0, 2}};
    auto out = rmtc_bot::redirect_avoid_plan(RectPlan(1, 1, 3, 3, 200), 1, 2,
                                             18, 18, blocked, c);
    CHECK(out.action == "move" && out.direction == "up");  // (1,1) 距离 1，枚举靠前
  }
  {  // 20. 区内四向全被封（障碍+出界）→ wait
    ActionChoice c = ActionChoice::Move("right", "采集");  // (1,0) 在区内
    const std::set<std::pair<int, int>> blocked = {{1, 0}, {0, 1}};
    auto out = rmtc_bot::redirect_avoid_plan(RectPlan(0, 0, 5, 5, 200), 0, 0,
                                             18, 18, blocked, c);
    CHECK(out.action == "wait");
  }
  {  // 21. 区外、安全方向全被障碍占、剩余方向进区/出界 → wait（宁可不走）
    ActionChoice c = ActionChoice::Move("right", "采集");  // (1,8) 在区内
    const std::set<std::pair<int, int>> blocked = {{0, 7}, {0, 9}};
    auto out = rmtc_bot::redirect_avoid_plan(RectPlan(1, 1, 16, 16, 200), 0, 8,
                                             18, 18, blocked, c);
    CHECK(out.action == "wait");
  }
  {  // 22. 单列矩形 x0==x1：区内横向一步即出（枚举序 right 先于 left）
    ActionChoice c = ActionChoice::Move("up", "采集");
    auto out = rmtc_bot::redirect_avoid_plan(RectPlan(2, 0, 2, 17, 200), 2, 5,
                                             18, 18, no_block, c);
    CHECK(out.action == "move" && out.direction == "right");
  }
  {  // 23. 点位区：候选会踩上点 → 换方向
    EventPlan pp;
    pp.avoid = true;
    pp.points = {{5, 5}};
    pp.until_tick = 200;
    ActionChoice c = ActionChoice::Move("right", "采集");  // (5,5) 是点位
    auto out = rmtc_bot::redirect_avoid_plan(pp, 4, 5, 18, 18, no_block, c);
    CHECK(out.action == "move" && out.direction == "up");
  }
  {  // 24. 未知方向 + 区外 → 原样（dest 按原地处理，不被误改）
    ActionChoice c = ActionChoice::Move("diag", "采集");
    auto out = rmtc_bot::redirect_avoid_plan(RectPlan(5, 5, 6, 6, 200), 1, 1,
                                             18, 18, no_block, c);
    CHECK(out.action == "move" && out.direction == "diag");
  }
}

}  // namespace

int main() {
  std::printf("==== combat_llm_guide LLM 意图翻译层离线自检 ====\n");
  TestPrepare();
  TestPriority();
  TestPlan();
  TestRedirect();
  if (g_fail == 0) {
    std::printf("==== 全部通过 ====\n");
    return 0;
  }
  std::printf("==== 失败 %d 项 ====\n", g_fail);
  return 1;
}
