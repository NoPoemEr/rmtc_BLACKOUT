#include "rmtc_bot/bot_node.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace rmtc_bot {

namespace {
// 话题 QoS 与协议保持一致：
// 感知/动作：RELIABLE + VOLATILE + KEEP_LAST(1)
// 注册：     RELIABLE + TRANSIENT_LOCAL + KEEP_LAST(1)
rclcpp::QoS qos_perception_action() {
  return rclcpp::QoS(rclcpp::KeepLast(1))
      .reliability(rclcpp::ReliabilityPolicy::Reliable)
      .durability(rclcpp::DurabilityPolicy::Volatile);
}

rclcpp::QoS qos_registration() {
  return rclcpp::QoS(rclcpp::KeepLast(1))
      .reliability(rclcpp::ReliabilityPolicy::Reliable)
      .durability(rclcpp::DurabilityPolicy::TransientLocal);
}

// 规则 §12.2：队名按 UTF-8 编码为 1–96 字节，不得包含换行或回车。
// 超限按 UTF-8 码点边界截断、换行回车剔除、空名回退默认；修改过置 modified。
// 防御性编程：正常输入根本走不到这些分支，但规则要求的合规行为必须显式实现。
std::string sanitize_team_name(const std::string &raw, bool &modified) {
  std::string s;
  s.reserve(raw.size());
  for (unsigned char c : raw) {
    if (c == '\r' || c == '\n') {
      modified = true;
      continue;
    }
    s.push_back(static_cast<char>(c));
  }
  if (s.size() > 96) {
    modified = true;
    std::size_t keep = 96;
    while (keep > 0 && (static_cast<unsigned char>(s[keep]) & 0xC0) == 0x80) {
      --keep;  // 回退到码点边界，避免截断半个多字节字符
    }
    s.resize(keep);
  }
  if (s.empty()) {
    modified = true;
    s = "RMTC_BOT";
  }
  return s;
}
}  // namespace

BotNode::BotNode(const std::string &seat, const std::string &team_name)
    : Node("rmtc_bot_" + seat), seat_(seat), team_name_(team_name) {
  // 席位必须是 A–D 大写单字符；非法直接抛异常，杜绝非法席位对象被构造
  if (seat_.size() != 1 || seat_[0] < 'A' || seat_[0] > 'D') {
    throw std::invalid_argument("席位必须为 A/B/C/D");
  }

  // 队名合规校验（规则 §12.2：1–96 字节 UTF-8、无换行回车）
  bool name_modified = false;
  const std::string safe_name = sanitize_team_name(team_name_, name_modified);
  if (name_modified) {
    RCLCPP_WARN(get_logger(), "队名按规则清理（1–96 字节 UTF-8、无换行回车）：原=[%s] 现=[%s]",
                team_name_.c_str(), safe_name.c_str());
  }
  team_name_ = safe_name;

  // 订阅本席位感知
  perception_sub_ = create_subscription<std_msgs::msg::String>(
      "/team_" + seat_ + "/perception", qos_perception_action(),
      [this](const std_msgs::msg::String::SharedPtr msg) { on_perception(msg); });

  // 发布本席位动作
  action_pub_ = create_publisher<std_msgs::msg::String>(
      "/team_" + seat_ + "/action", qos_perception_action());

  // 发布本席位队伍名（TRANSIENT_LOCAL：后接入的订阅者也能收到，故注册后保持运行）
  registration_pub_ = create_publisher<std_msgs::msg::String>(
      "/team_" + seat_ + "/registration", qos_registration());

  publish_registration();

  // 创建策略（decision_config.json 的 decision_type 决定档位；M1 默认 wait）
  strategy_ = make_strategy();
  // LLM 触发/提示词管道：仅 combat 档创建（10/29 用户拍板：阶段一决策不消费
  // LLM 结论，关闭调用省配额、降网关风险）；collect/wait 档不创建 → 零调用、零开销。
  if (std::string(strategy_->name()) == "combat") {
    brain_ = std::make_unique<LlmBrain>();
  }
  RCLCPP_INFO(get_logger(), "节点启动：席位=%s 队名=%s 策略=%s%s", seat_.c_str(),
              team_name_.c_str(), strategy_->name(),
              brain_ ? (brain_->client_enabled() ? " LLM=已启用" : " LLM=未配置(仅本地策略)")
                     : " LLM=off(本档不启用)");
}

void BotNode::on_perception(const std_msgs::msg::String::SharedPtr msg) {
  std::vector<std::string> warnings;
  Perception p;
  if (!parse_perception(msg->data, p, warnings)) {
    // 核心字段不可用：不发布动作（裁判按 WAIT 结算），绝不让进程崩溃
    for (const auto &w : warnings) {
      RCLCPP_WARN(get_logger(), "感知解析失败：%s", w.c_str());
    }
    return;
  }
  for (const auto &w : warnings) {
    RCLCPP_WARN(get_logger(), "感知字段警告：%s", w.c_str());
  }

  // 旧/重复 tick 丢弃（M1 修复）：按场次记录最后处理到的 tick。
  // 同一场次 tick 不增长 → 丢弃；曾离开的场次（换场后的迟到帧）→ 丢弃。
  // 这样旧场迟到帧不会再触发换场逻辑，把当前场次回翻成旧场（E_SESSION + 配额浪费）。
  auto tit = tick_by_session_.find(p.session_id);
  if (tit != tick_by_session_.end() && p.tick <= tit->second) {
    RCLCPP_DEBUG(get_logger(), "忽略旧/重复 tick=%d（场次 %s 已处理到 %d）",
                 p.tick, p.session_id.c_str(), tit->second);
    return;
  }
  if (p.session_id != session_id_ && left_sessions_.count(p.session_id) != 0) {
    RCLCPP_DEBUG(get_logger(), "忽略已离开场次 %s 的迟到帧（tick=%d）",
                 p.session_id.c_str(), p.tick);
    return;
  }
  // 换场：清空上一场状态并重新注册
  if (p.session_id != session_id_) {
    handle_new_session(p.session_id);
  }
  tick_by_session_[p.session_id] = p.tick;
  last_tick_ = p.tick;

  // 打印解析结果：验证解析正确性与双向通信的日志素材
  log_perception(p);

  // 决策：LLM 触发管道先行（触发则异步提交提示词），再把最近一次结论传给策略
  // （brain 为空 → 传 nullptr，与 M1 行为一致；结论为失败态时策略自行降级）。
  // 异常兜底（S9）：决策链任何异常都降级为 wait，绝不让异常沿 spin 抛出拖死节点。
  ActionChoice choice;
  try {
    if (brain_) {
      brain_->observe(p);
    }
    choice = strategy_->decide(p, brain_ ? brain_->latest_result() : nullptr);
  } catch (const std::exception &e) {
    RCLCPP_ERROR(get_logger(), "决策异常，本Tick降级 wait：%s", e.what());
    choice = ActionChoice::Wait("决策异常，降级 wait");
  }
  RCLCPP_INFO(get_logger(), "本Tick动作: %s %s（%s）", choice.action.c_str(),
              choice.direction.c_str(), choice.reason.c_str());
  publish_action(p.tick, choice);
}

void BotNode::log_perception(const Perception &p) {
  const SelfState &s = p.self;
  std::string pos =
      s.has_position ? "[" + std::to_string(s.x) + "," + std::to_string(s.y) +
                           "]"
                     : "?";
  std::string hp = s.has_hp ? std::to_string(s.hp) : "?";
  std::string alive = s.has_alive ? (s.alive ? "存活" : "阵亡") : "?";
  RCLCPP_INFO(get_logger(),
              "tick=%d 地图=%dx%d 半径=%d pos=%s hp=%s %s 实体=%zu 任务=%zu "
              "事件文本=%zu字",
              p.tick, p.map_width, p.map_height, p.vision_radius, pos.c_str(),
              hp.c_str(), alive.c_str(), p.entities.size(), p.missions.size(),
              p.event_text.size());

  // 实体明细（验证 9×9 图解析；策略接入后可降为 DEBUG）
  for (const auto &e : p.entities) {
    std::string loc = "?";
    if (e.has_absolute) {
      loc = "[" + std::to_string(e.abs_x) + "," + std::to_string(e.abs_y) + "]";
    }
    std::string extra;
    if (e.has_value) extra += " value=" + std::to_string(e.value);
    if (e.has_hp) extra += " hp=" + std::to_string(e.hp);
    RCLCPP_INFO(get_logger(), "  实体 type=%s id=%s 位置%s(相对%+d,%+d)%s",
                e.type.c_str(), e.id.c_str(), loc.c_str(), e.rel_dx, e.rel_dy,
                extra.c_str());
  }

  // 任务明细
  for (const auto &m : p.missions) {
    std::string detail;
    if (m.has_target) detail += " target=" + std::to_string(m.target);
    if (m.has_position) {
      detail += " pos=[" + std::to_string(m.pos_x) + "," +
                std::to_string(m.pos_y) + "]";
    }
    RCLCPP_INFO(get_logger(), "  任务 id=%s type=%s%s", m.id.c_str(),
                m.type.c_str(), detail.c_str());
  }

  // 上一 Tick 回执（双向通信确认的关键输出）
  if (s.has_last_action) {
    std::string receipt = "tick=" + std::to_string(s.last_tick) +
                          " action=" + s.last_action +
                          " moved=" + (s.last_moved ? "true" : "false");
    if (s.has_last_direction) {
      receipt += " dir=" + s.last_effective_direction;
    }
    if (s.has_last_error) {
      receipt += " error=" + s.last_error;
    }
    RCLCPP_INFO(get_logger(), "上Tick回执: %s", receipt.c_str());
  } else {
    RCLCPP_INFO(get_logger(), "上Tick回执: 无（开局或字段缺失）");
  }

  // 本队得分（监控非法动作与任务完成）
  RCLCPP_INFO(get_logger(),
              "得分 资源=%d 伤害=%.1f 任务完成=%d 非法动作=%d",
              p.score.resource_value, p.score.effective_damage,
              p.score.mission_completed, p.score.invalid_actions);
}

void BotNode::publish_action(int tick, const ActionChoice &choice) {
  // 协议层兜底校验：move/attack 必须带合法方向；非法动作一律降级 wait
  // （策略写错时保证"每 Tick 发一条、发就发合规的"，不会白刷非法动作）
  std::string action = choice.action;
  std::string direction = choice.direction;
  const bool needs_direction = (action == "move" || action == "attack");
  if (needs_direction &&
      direction != "up" && direction != "down" &&
      direction != "left" && direction != "right") {
    RCLCPP_WARN(get_logger(),
                "策略输出非法动作 %s %s，本Tick降级为 wait",
                action.c_str(), direction.c_str());
    action = "wait";
    direction.clear();
  } else if (action != "move" && action != "attack" &&
             action != "defend" && action != "wait") {
    RCLCPP_WARN(get_logger(), "策略输出未知动作 %s，本Tick降级为 wait",
                action.c_str());
    action = "wait";
    direction.clear();
  }

  nlohmann::json j = {
      {"tick", tick},               // 整数，原样回填
      {"session_id", session_id_},  // 字符串，原样回填
      {"action", action}            // 字段必须小写
  };
  if (needs_direction && action != "wait") {
    j["direction"] = direction;  // 仅 move/attack 携带
  }
  auto msg = std_msgs::msg::String();
  msg.data = j.dump();
  action_pub_->publish(msg);
}

void BotNode::publish_registration() {
  const nlohmann::json reg = {
      {"team", seat_},
      {"name", team_name_},
  };
  auto msg = std_msgs::msg::String();
  msg.data = reg.dump();
  registration_pub_->publish(msg);
}

void BotNode::handle_new_session(const std::string &session_id) {
  RCLCPP_INFO(get_logger(), "检测到新场次，清空缓存并重新注册: %s",
              session_id.c_str());
  // M1 修复：记下离开的场次——其后到达的迟到帧一律丢弃（见 on_perception）
  if (!session_id_.empty() && session_id_ != session_id) {
    left_sessions_.insert(session_id_);
  }
  session_id_ = session_id;
  last_tick_ = -1;
  strategy_->on_new_session(session_id);  // 清空策略内部的地图记忆/规划缓存
  if (brain_) {
    brain_->on_new_session(session_id);  // 清空 LLM 触发状态与结论缓存
  }
  publish_registration();
}

}  // namespace rmtc_bot
