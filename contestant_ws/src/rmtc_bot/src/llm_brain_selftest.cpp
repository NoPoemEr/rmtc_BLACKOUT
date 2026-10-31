// LLM 触发/提示词/结论持有管道自检（不依赖 ROS/网络，可独立运行）：
//   ros2 run rmtc_bot llm_brain_selftest
// 场景覆盖 docs/llm_prompt_design.md §2 输入构造与 §3 触发时机（触发判定是
// 纯函数，全部离线可测）；末尾一组用禁用/不可达客户端验证 observe() 接线。
// 退出码 0 = 全部通过，1 = 有失败。

#include <cstdio>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rmtc_bot/llm_brain.hpp"

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

using rmtc_bot::BrainState;
using rmtc_bot::LlmBrain;
using rmtc_bot::LlmBrainParams;
using rmtc_bot::LlmClient;
using rmtc_bot::Perception;
using rmtc_bot::TriggerReason;

// 造一帧最小感知（字段直接填，不经过协议解析）
Perception make_p(const std::string& event, int tick = 0) {
  Perception p;
  p.tick = tick;
  p.session_id = "sess-1";
  p.map_width = 20;
  p.map_height = 20;
  p.self.has_position = true;
  p.self.x = 8;
  p.self.y = 8;
  p.self.has_hp = true;
  p.self.hp = 100;
  p.self.has_alive = true;
  p.self.alive = true;
  p.score.resource_value = 30;
  p.score.mission_completed = 1;
  p.mission_text = "采集资源并到达目标点";
  rmtc_bot::Mission m1;
  m1.id = "m1";
  m1.type = "resource_high";
  m1.has_target = true;
  m1.target = 2;
  rmtc_bot::Mission m2;
  m2.id = "m2";
  m2.type = "reach";
  m2.has_target = true;
  m2.target = 1;
  m2.has_position = true;
  m2.pos_x = 3;
  m2.pos_y = 3;
  p.missions = {m1, m2};
  p.score.mission_completed_ids = {"m0"};
  p.event_text = event;
  return p;
}

// ---- 触发判定纯函数 ----

TriggerReason eval(const BrainState& st, int tick, const std::string& event,
                   const std::string& sig, LlmBrainParams params = {}) {
  return rmtc_bot::evaluate_trigger(st, tick, event, sig,
                                    /*client_enabled=*/true, params);
}

void test_trigger_session_start() {
  BrainState st;
  check(eval(st, 0, "", "sig") == TriggerReason::SESSION_START,
        "首帧触发 SESSION_START（无事件无任务也触发）");
  check(rmtc_bot::evaluate_trigger(st, 0, "公告", "sig", false, {}) ==
            TriggerReason::NONE,
        "客户端禁用: 一律 NONE");
  check(rmtc_bot::evaluate_trigger(st, 0, "公告", "sig", true, {}) ==
            TriggerReason::SESSION_START,
        "客户端可用: 首帧照常触发");

  // 首帧触发不受 min_gap 约束（last_submit_tick=-1，tick=0 间隔只有 1）
  BrainState st2;
  st2.last_submit_tick = -1;
  check(eval(st2, 0, "", "sig") == TriggerReason::SESSION_START,
        "首帧不受最小间隔约束");
}

void test_trigger_event_change() {
  LlmBrainParams p;
  // 首帧提交后：tick=4（间隔 4 < min_gap 5）→ 不触发
  BrainState st;
  st.seen_any = true;
  st.last_submit_tick = 0;
  st.last_event_text = "旧公告";
  st.last_mission_sig = "sig";
  check(eval(st, 4, "新公告", "sig", p) == TriggerReason::NONE,
        "事件变化但最小间隔内: NONE");
  // tick=6（间隔 6 ≥ 5）→ EVENT_CHANGE
  check(eval(st, 6, "新公告", "sig", p) == TriggerReason::EVENT_CHANGE,
        "事件变化且间隔足够: EVENT_CHANGE");
  // 事件没变（相等比较，非语义）→ 不触发
  check(eval(st, 6, "旧公告", "sig", p) == TriggerReason::NONE,
        "事件文本未变: NONE");
  // 空事件（无公告）→ 不算变化
  check(eval(st, 6, "", "sig", p) == TriggerReason::NONE,
        "空事件文本不算变化: NONE");
}

void test_trigger_mission_change() {
  LlmBrainParams p;
  BrainState st;
  st.seen_any = true;
  st.last_submit_tick = 0;
  st.last_mission_sig = "old";
  check(eval(st, 10, "", "new", p) == TriggerReason::MISSION_CHANGE,
        "任务结构变化: MISSION_CHANGE");
  check(eval(st, 10, "", "old", p) == TriggerReason::NONE,
        "任务结构未变: NONE");
}

void test_trigger_periodic() {
  LlmBrainParams p;  // review_every_ticks=25
  BrainState st;
  st.seen_any = true;
  st.last_submit_tick = 0;
  st.last_mission_sig = "sig";
  check(eval(st, 20, "", "sig", p) == TriggerReason::NONE,
        "未到复盘周期: NONE");
  check(eval(st, 25, "", "sig", p) == TriggerReason::PERIODIC,
        "到复盘周期: PERIODIC");
}

void test_trigger_backoff() {
  LlmBrainParams p;  // backoff=10
  BrainState st;
  st.seen_any = true;
  st.last_submit_tick = 0;
  st.backoff_until_tick = 50;
  st.last_mission_sig = "sig";
  check(eval(st, 30, "新公告", "sig", p) == TriggerReason::NONE,
        "退避期内: 事件变化也不触发");
  check(eval(st, 50, "新公告", "sig", p) == TriggerReason::EVENT_CHANGE,
        "退避结束（tick==backoff_until）: 恢复触发");
}

void test_trigger_budget() {
  LlmBrainParams p;  // max=30, reserve=3 → 软区 27 起
  // 硬顶：submitted == max → 事件变化也 NONE
  BrainState st;
  st.seen_any = true;
  st.last_submit_tick = 0;
  st.submitted = 30;
  check(eval(st, 50, "新公告", "sig", p) == TriggerReason::NONE,
        "达到全场上限: 事件也 NONE");

  // 软区：submitted=27 → 只保事件类
  BrainState st2;
  st2.seen_any = true;
  st2.last_submit_tick = 0;
  st2.submitted = 27;
  st2.last_mission_sig = "sig";
  check(eval(st2, 50, "新公告", "sig", p) == TriggerReason::EVENT_CHANGE,
        "预留区内: 事件变化仍触发");
  check(eval(st2, 50, "", "new", p) == TriggerReason::NONE,
        "预留区内: 任务变化不触发");
  check(eval(st2, 50, "", "sig", p) == TriggerReason::NONE,
        "预留区内: 周期复盘不触发");
}

// ---- 提示词构造 ----

void test_system_message() {
  const std::string s = rmtc_bot::build_system_message(20, 15);
  check(s.find("20x15") != std::string::npos, "system: 地图尺寸代入 {W}x{H}");
  check(s.find("{W}") == std::string::npos && s.find("{H}") == std::string::npos,
        "system: 占位符全部替换");
  check(s.find("goals") != std::string::npos &&
            s.find("event_plan") != std::string::npos,
        "system: 输出 schema 键齐备");
  check(s.find("只输出一个 JSON") != std::string::npos, "system: 只输出 JSON 约束");
  check(s.find("avoid:true") != std::string::npos &&
            s.find("avoid:false") != std::string::npos,
        "system: 危险区/增益点位两种表达齐备（§6 已定稿）");
  check(s.find("事件全表") == std::string::npos &&
            s.find("分值公式") == std::string::npos,
        "system: 不给事件全表/分值公式（防过拟合，§6 已定稿）");
}

void test_user_message() {
  const char* k_event = "第 12 Tick 预告：\n左上角【幻术】区域将生效，移动方向颠倒。";
  Perception p = make_p(k_event, 12);
  const std::string u = rmtc_bot::build_user_message(p);

  check(u.find(k_event) != std::string::npos, "user: 事件原文整段原样嵌入");
  check(u.find("tick=12") != std::string::npos, "user: tick");
  check(u.find("pos=[8,8]") != std::string::npos, "user: 本车坐标");
  check(u.find("hp=100") != std::string::npos, "user: 血量");
  check(u.find("已采价值=30") != std::string::npos, "user: 已采价值");
  check(u.find("任务类型=resource_high (id=m1 target=2)") != std::string::npos,
        "user: 任务结构（类型在前、含 target）");
  check(u.find("pos=[3,3]") != std::string::npos, "user: reach 目标点");
  check(u.find("已完成任务 id: m0") != std::string::npos, "user: 已完成任务列表");
  check(u.find("采集资源并到达目标点") != std::string::npos, "user: 任务原文");

  Perception empty = make_p("", 5);
  check(rmtc_bot::build_user_message(empty).find("(无)") != std::string::npos,
        "user: 无公告时写入 (无)");
}

void test_mission_signature() {
  Perception a = make_p("", 0);
  Perception b = make_p("", 0);
  check(rmtc_bot::mission_signature(a) == rmtc_bot::mission_signature(b),
        "签名: 相同任务同签名");
  b.score.mission_completed_ids.push_back("m1");
  check(rmtc_bot::mission_signature(a) != rmtc_bot::mission_signature(b),
        "签名: 完成任务变化→签名变化");
  Perception c = make_p("", 0);
  c.missions[0].has_target = false;
  check(rmtc_bot::mission_signature(a) != rmtc_bot::mission_signature(c),
        "签名: 任务字段变化→签名变化");
}

// ---- LlmBrain 接线（客户端不可达/禁用，不依赖网络） ----

void test_brain_disabled_client() {
  auto client = std::make_shared<LlmClient>("", "");
  LlmBrain brain(client);
  check(!brain.client_enabled(), "禁用客户端: enabled=false");
  Perception p = make_p("新公告", 0);
  brain.observe(p);
  check(brain.submitted() == 0, "禁用客户端: 不提交任何调用");
  check(brain.latest_result() != nullptr, "禁用客户端: latest_result 指针稳定");
}

void test_brain_submit_and_session_reset() {
  // 指向不可达端口：submit 入队成功（同步判定），后台调用快速失败，
  // 不阻塞、不崩溃；只断言同步可见的提交计数与换场清零。
  // backoff=0：失败到达的时点不确定（连接拒绝极快），退避窗口设为 0
  // 保证断言确定性；退避行为本身已由 test_trigger_backoff 纯函数覆盖。
  auto client = std::make_shared<LlmClient>(
      "http://127.0.0.1:1", "test-token", /*model=*/"", /*max_tokens=*/100,
      /*timeout_ms=*/500, /*max_retries=*/0, /*backoff=*/100);
  LlmBrainParams params;
  params.backoff_ticks_after_failure = 0;
  LlmBrain brain(client, params);
  check(brain.client_enabled(), "可达配置客户端: enabled=true");

  Perception p0 = make_p("", 0);
  brain.observe(p0);
  check(brain.submitted() == 1, "首帧: 提交 1 次（SESSION_START）");

  Perception p6 = make_p("第 6 Tick 新公告", 6);
  brain.observe(p6);
  check(brain.submitted() == 2, "tick6 事件变化: 提交第 2 次");

  // 同一帧重复观察：最小间隔内 → 不再提交
  brain.observe(p6);
  check(brain.submitted() == 2, "同帧重复观察: 不重复提交");

  brain.on_new_session("sess-2");
  check(brain.submitted() == 0, "换场: 提交计数清零");
  Perception q0 = make_p("", 0);
  q0.session_id = "sess-2";
  brain.observe(q0);
  check(brain.submitted() == 1, "新场首帧: 重新触发 SESSION_START");

  check(brain.last_trigger() != nullptr, "last_trigger 名可读");

  // M5 回归：换场必须清空缓存的结论副本——新场首帧不得读到上一场的 LLM 结论。
  // 等不可达客户端快速失败产生第一个结果，再观察消费进 cached_result_。
  for (int i = 0; i < 100 && client->done_count() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  Perception p8 = make_p("第 8 Tick 新公告", 8);
  p8.session_id = "sess-2";
  brain.observe(p8);  // 消费完成结果 → cached_result_ 变为失败态（非 DISABLED）
  const rmtc_bot::LlmResult* before = brain.latest_result();
  check(before != nullptr && before->status != rmtc_bot::LlmResult::Status::DISABLED,
        "M5: 换场前缓存了最近完成结果（非 DISABLED）");
  brain.on_new_session("sess-3");
  check(brain.latest_result()->status == rmtc_bot::LlmResult::Status::DISABLED,
        "M5: 换场清空结论缓存（新场不再读到上一场结论）");
}

}  // namespace

int main() {
  test_trigger_session_start();
  test_trigger_event_change();
  test_trigger_mission_change();
  test_trigger_periodic();
  test_trigger_backoff();
  test_trigger_budget();
  test_system_message();
  test_user_message();
  test_mission_signature();
  test_brain_disabled_client();
  test_brain_submit_and_session_reset();

  if (g_failures == 0) {
    std::printf("\n全部通过（%d 项）。\n", g_checks);
    return 0;
  }
  std::printf("\n%d 项失败（共 %d 项）。\n", g_failures, g_checks);
  return 1;
}
