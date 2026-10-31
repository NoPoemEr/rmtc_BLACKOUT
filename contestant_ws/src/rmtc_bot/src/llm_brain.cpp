// llm_brain.cpp —— LLM 触发/提示词/结论持有管道（docs/llm_prompt_design.md §2/§3）
//
// 规则红线提醒：本文件对 event_text 只做整串相等比较（变化检测），
// 不含任何关键词/正则/模板解析分支；内容整段原样进入 prompt 交给 LLM。

#include "rmtc_bot/llm_brain.hpp"

#include <cstdio>

namespace rmtc_bot {

const char *trigger_reason_name(TriggerReason r) {
  switch (r) {
    case TriggerReason::SESSION_START: return "SESSION_START";
    case TriggerReason::EVENT_CHANGE: return "EVENT_CHANGE";
    case TriggerReason::MISSION_CHANGE: return "MISSION_CHANGE";
    case TriggerReason::PERIODIC: return "PERIODIC";
    case TriggerReason::NONE: return "NONE";
  }
  return "?";
}

TriggerReason evaluate_trigger(const BrainState &st, int tick,
                               const std::string &event_text,
                               const std::string &mission_sig,
                               bool client_enabled,
                               const LlmBrainParams &params) {
  // 客户端不可用：任何触发都无从谈起（enabled 语义已含配额/鉴权永久降级）
  if (!client_enabled) {
    return TriggerReason::NONE;
  }

  // 新场次首帧：最高优先，保证尽早有一次调用（AI 资格 ≥1 成功/场）
  if (!st.seen_any) {
    return TriggerReason::SESSION_START;
  }

  // 失败退避期内：静默（退避只压业务触发，不影响上面已经过的首帧判定）
  if (tick < st.backoff_until_tick) {
    return TriggerReason::NONE;
  }

  // 两次提交最小间隔（事件预告提前 5 Tick，min_gap 内必有一次窗口）
  if (tick - st.last_submit_tick < params.min_gap_ticks) {
    return TriggerReason::NONE;
  }

  // 全场调用上限（硬顶）
  if (st.submitted >= params.max_calls_per_match) {
    return TriggerReason::NONE;
  }

  // 事件公告变化：最高优先业务触发（预告时效 5 Tick，必须尽快问）
  if (!event_text.empty() && event_text != st.last_event_text) {
    return TriggerReason::EVENT_CHANGE;
  }

  // 预算护栏：接近上限时只保事件类触发
  if (st.submitted >= params.max_calls_per_match - params.reserve_for_events) {
    return TriggerReason::NONE;
  }

  // 任务结构变化：重新排 goals
  if (mission_sig != st.last_mission_sig) {
    return TriggerReason::MISSION_CHANGE;
  }

  // 周期复盘兜底
  if (tick - st.last_submit_tick >= params.review_every_ticks) {
    return TriggerReason::PERIODIC;
  }

  return TriggerReason::NONE;
}

std::string mission_signature(const Perception &p) {
  std::string sig;
  for (const auto &m : p.missions) {
    sig += m.id + ':' + m.type;
    if (m.has_target) {
      sig += ':' + std::to_string(m.target);
    }
    sig += ';';
  }
  sig += "|done:";
  for (const auto &id : p.score.mission_completed_ids) {
    sig += id + ',';
  }
  return sig;
}

std::string build_system_message(int map_w, int map_h) {
  // 默认稿（docs/llm_prompt_design.md §2；§6 讨论点 10/21 已拍板：
  //   ① 不给分值公式/事件全表（防过拟合练习配置，正式赛事件可能不同）
  //   ② 复盘周期 N=25 维持（500 Tick 练习场 ≈ 29 次总调用，上限内）
  //   ③ 启用 goto 点位：增益点位用 points+avoid:false 表达）
  std::string s =
      "你是 RMTC 机甲大师图灵赛战车决策助手。地图尺寸 {W}x{H}，坐标 [x,y]，"
      "x 向右、y 向下。事件公告可能预告未来事件（提前 5 Tick 生效）。\n"
      "你只输出一个 JSON 对象，不要输出任何多余文字：\n"
      "{\"goals\":[\"任务类型按当前优先级排序\"],"
      "\"event_plan\":{\"rect\":[[x0,y0],[x1,y1]],\"avoid\":true,"
      "\"until_tick\":N,\"reason\":\"一句话\"}或null,"
      "\"summary\":\"一句话\"}\n"
      "goals 只能填下面任务行\"任务类型\"栏的值（即 type= 后的值，如 "
      "normal_resources / high_resources / reach），不要填任务 id，"
      "按当前优先级排序；无事件或无需应对时 event_plan 为 null；"
      "event_plan 带 until_tick 表示该结论的有效截止 Tick。"
      "表达危险区域（爆炸/火线等预告区）用 rect+avoid:true；"
      "表达建议前往的增益点位（如回血点）用 points+avoid:false，一次只给一种。";

  // 占位符替换 {W}/{H}
  auto replace = [&s](const std::string &key, int v) {
    std::size_t pos = 0;
    const std::string val = std::to_string(v);
    while ((pos = s.find(key, pos)) != std::string::npos) {
      s.replace(pos, key.size(), val);
      pos += val.size();
    }
  };
  replace("{W}", map_w);
  replace("{H}", map_h);
  return s;
}

std::string build_user_message(const Perception &p) {
  const SelfState &s = p.self;
  std::string u;

  // 【任务】：原文 + 结构化 + 已完成列表
  u += "【任务】\n";
  if (!p.mission_text.empty()) {
    u += "原文: " + p.mission_text + "\n";
  }
  for (const auto &m : p.missions) {
    u += "  任务类型=" + m.type + " (id=" + m.id;
    if (m.has_target) {
      u += " target=" + std::to_string(m.target);
    }
    if (m.has_position) {
      u += " pos=[" + std::to_string(m.pos_x) + "," +
           std::to_string(m.pos_y) + "]";
    }
    u += ")\n";
  }
  if (!p.score.mission_completed_ids.empty()) {
    u += "已完成任务 id: ";
    for (const auto &id : p.score.mission_completed_ids) {
      u += id + " ";
    }
    u += "\n";
  }

  // 【本车状态】：最小集（位置/血量/存活/已采价值/任务完成数）
  u += "【本车状态】\n";
  u += "tick=" + std::to_string(p.tick);
  if (s.has_position) {
    u += " pos=[" + std::to_string(s.x) + "," + std::to_string(s.y) + "]";
  }
  if (s.has_hp) {
    u += " hp=" + std::to_string(s.hp);
  }
  if (s.has_alive) {
    u += s.alive ? " 存活" : " 阵亡";
    if (!s.alive && s.respawn_in > 0) {
      u += "(距复活" + std::to_string(s.respawn_in) + "Tick)";
    }
  }
  u += " 已采价值=" + std::to_string(p.score.resource_value);
  u += " 任务完成数=" + std::to_string(p.score.mission_completed);
  u += "\n";

  // 【事件公告原文】：整段原样嵌入，一字不改（规则红线）
  u += "【事件公告原文】\n";
  if (p.event_text.empty()) {
    u += "(无)";
  } else {
    u += p.event_text;
  }
  u += "\n";

  return u;
}

// ---- LlmBrain ----

LlmBrain::LlmBrain() : LlmBrain(std::make_shared<LlmClient>()) {}

LlmBrain::LlmBrain(std::shared_ptr<LlmClient> client, LlmBrainParams params)
    : client_(std::move(client)), params_(params) {
  last_done_count_ = client_->done_count();
}

void LlmBrain::observe(const Perception &p) {
  // 1) 消费客户端最新完成结果：缓存副本 + "新"失败结果 → 开启退避窗口
  const std::uint64_t done = client_->done_count();
  if (done != last_done_count_) {
    last_done_count_ = done;
    cached_result_ = client_->latest_result();
    if (is_failure_status(cached_result_.status)) {
      st_.backoff_until_tick = p.tick + params_.backoff_ticks_after_failure;
      std::fprintf(stderr,
                   "[llm_brain] 调用失败（http=%d err=%s），退避到 tick %d\n",
                   cached_result_.http_code, cached_result_.error_code.c_str(),
                   st_.backoff_until_tick);
    }
  }

  // 2) 触发判定（纯函数）
  const std::string sig = mission_signature(p);
  const TriggerReason reason =
      evaluate_trigger(st_, p.tick, p.event_text, sig, client_->enabled(),
                       params_);
  last_trigger_ = reason;
  if (reason == TriggerReason::NONE) {
    return;
  }

  // 3) 构造 prompt 并入队（绝不阻塞）
  const std::string prompt =
      build_system_message(p.map_width, p.map_height) + "\n\n" +
      build_user_message(p);
  if (prompt == last_prompt_) {
    return;  // 内容与上次完全一致：合并去重（设计稿 §3 节流）
  }
  if (!client_->submit(prompt)) {
    std::fprintf(stderr, "[llm_brain] 提交失败（禁用/队满），跳过本次触发\n");
    return;
  }

  // 4) 提交成功后更新触发状态（变化检测基准随提交走，防止间隔期内重复触发）
  st_.seen_any = true;
  st_.submitted += 1;
  st_.last_submit_tick = p.tick;
  st_.last_event_text = p.event_text;
  st_.last_mission_sig = sig;
  last_prompt_ = prompt;
  std::fprintf(stderr,
               "[llm_brain] tick=%d 触发=%s 本场第 %d 次调用\n",
               p.tick, trigger_reason_name(reason), st_.submitted);
}

void LlmBrain::on_new_session(const std::string &session_id) {
  (void)session_id;
  st_ = BrainState{};
  last_prompt_.clear();
  last_trigger_ = TriggerReason::NONE;
  // M5 修复：清空结论缓存副本——新场首帧不得读到上一场的 LLM 结论
  // （否则 10/28 接入消费后，换场首帧策略会拿旧场结论做决策）。
  cached_result_ = LlmResult{};
  // last_done_count_ 保留：客户端统计是进程级，换场不重置
}

bool LlmBrain::client_enabled() const { return client_->enabled(); }

bool LlmBrain::quota_degraded() const { return client_->quota_degraded(); }

const char *LlmBrain::last_trigger() const {
  return trigger_reason_name(last_trigger_);
}

bool LlmBrain::is_failure_status(LlmResult::Status s) {
  switch (s) {
    case LlmResult::Status::NETWORK_ERROR:
    case LlmResult::Status::TIMEOUT:
    case LlmResult::Status::HTTP_ERROR:
    case LlmResult::Status::PARSE_ERROR:
    case LlmResult::Status::RATE_LIMITED:
      return true;
    case LlmResult::Status::SUCCESS:
    case LlmResult::Status::QUOTA_EXHAUSTED:  // 永久降级，enabled()=false 拦截
    case LlmResult::Status::AUTH_ERROR:       // 同上
    case LlmResult::Status::DISABLED:
      return false;
  }
  return false;
}

}  // namespace rmtc_bot
