// llm_advisor.cpp —— LLM 顾问层实现（纯函数）
//
// 合规（rules §8.1）：本文件不解析 event_text 内容，只把原文交给 LLM，
// 并解析 LLM 返回的结构化 JSON。

#include "rmtc_bot/llm_advisor.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>

#include <nlohmann/json.hpp>

namespace rmtc_bot {

namespace {

void add_warning(std::vector<std::string>& w, const std::string& s) {
  if (w.size() < 64) w.push_back(s);
}

EventType type_from_string(const std::string& s) {
  if (s == "explosion") return EventType::Explosion;
  if (s == "illusion") return EventType::Illusion;
  if (s == "heal") return EventType::HealZone;
  if (s == "fortress") return EventType::Fortress;
  if (s == "world_split") return EventType::WorldSplit;
  if (s == "chili") return EventType::Chili;
  if (s == "thorns") return EventType::Thorns;
  if (s == "vulnerable") return EventType::Vulnerable;
  if (s == "other") return EventType::Other;
  return EventType::Unknown;
}

const char* type_to_string(EventType t) {
  switch (t) {
    case EventType::Explosion: return "explosion";
    case EventType::Illusion: return "illusion";
    case EventType::HealZone: return "heal";
    case EventType::Fortress: return "fortress";
    case EventType::WorldSplit: return "world_split";
    case EventType::Chili: return "chili";
    case EventType::Thorns: return "thorns";
    case EventType::Vulnerable: return "vulnerable";
    case EventType::Other: return "other";
    default: return "unknown";
  }
}

// 有界 double 读取：类型错/非有限/越界 → 默认值 + 警告
double read_double(const nlohmann::json& j, const char* key, double dflt,
                   double lo, double hi, bool& ok,
                   std::vector<std::string>& warnings, const std::string& where) {
  auto it = j.find(key);
  if (it == j.end()) return dflt;
  if (!it->is_number()) {
    add_warning(warnings, where + ": " + key + " 非数值，忽略");
    return dflt;
  }
  const double v = it->get<double>();
  if (!std::isfinite(v) || v < lo || v > hi) {
    add_warning(warnings, where + ": " + key + " 越界，忽略");
    return dflt;
  }
  ok = true;
  return v;
}

// 有界 int 读取
long long read_int(const nlohmann::json& j, const char* key, long long dflt,
                   long long lo, long long hi, bool& ok,
                   std::vector<std::string>& warnings, const std::string& where) {
  auto it = j.find(key);
  if (it == j.end()) return dflt;
  if (!it->is_number_integer()) {
    add_warning(warnings, where + ": " + key + " 非整数，忽略");
    return dflt;
  }
  const long long v = it->get<long long>();
  if (v < lo || v > hi) {
    add_warning(warnings, where + ": " + key + " 越界，忽略");
    return dflt;
  }
  ok = true;
  return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// 提示词构造
// ---------------------------------------------------------------------------

std::string build_event_parse_prompt(const std::string& event_text, int map_w,
                                     int map_h, int current_tick,
                                     const DecisionParams& params) {
  std::ostringstream os;
  os << "你是比赛决策系统的事件理解模块。请阅读下面的随机事件公告原文，"
        "按输出 Schema 输出一个 JSON 对象，不要输出任何其他内容。\n"
     << "当前 Tick: " << current_tick << "；地图尺寸: " << map_w << "x"
     << map_h << "（x 范围 [0," << map_w << ")，y 范围 [0," << map_h
     << ")，坐标 [x,y]）。\n"
     << "场次总 Tick 数: " << params.match_ticks << "。\n\n"
     << "公告原文:\n\"\"\"\n"
     << event_text << "\n\"\"\"\n\n"
     << "输出 Schema（JSON）:\n"
     << "{\"type\": \"explosion|illusion|heal|fortress|world_split|chili|"
        "thorns|vulnerable|other\",\n"
     << " \"region\": {\"rect\": [x0,y0,x1,y1]} 或 {\"points\": [[x,y],...]} "
        "或 null,\n"
     << " \"numbers\": {\"damage\": 环境伤害数值, \"heal\": 回复数值, "
        "\"duration\": 持续 tick 数} 或 null,\n"
     << " \"timing\": {\"active_tick\": 生效 tick, \"until_tick\": 结束 tick"
        "（含）} 或 null,\n"
     << " \"reason\": \"一句话说明事件效果与影响区域\"}\n\n"
     << "要求：数值一律以公告原文为准，不得猜测或套用任何默认值；公告未提及的"
        "字段输出 null；region 无明确区域时输出 null。\n";
  return os.str();
}

std::string build_strategy_prompt(const Belief& b,
                                  const std::vector<GoalCandidate>& goals,
                                  const DecisionParams& params) {
  std::ostringstream os;
  os << "你是比赛决策系统的战术顾问。下面是本地程序整理好的局面摘要与候选"
        "方案（期望分已由本地按规则公式计算）。请基于摘要做高层判断，输出 "
        "JSON：\n"
     << "{\"rank\": [候选编号按优先级降序], \"weights\": {\"resource\": 乘数,"
        " \"task\": 乘数, \"combat\": 乘数, \"survival\": 乘数}, \"advice\": "
        "\"一句话战术建议\"}\n"
     << "weights 取值范围 [0.2, 5.0]，1.0 表示不调整；没有倾向就输出 1.0。\n\n"
     << "=== 局面摘要 ===\n"
     << "Tick: " << b.tick << " / " << params.match_ticks << "\n"
     << "我方: HP=" << (b.our_hp_known ? std::to_string(b.our_hp) : "?")
     << " 位置=(" << b.our_x << "," << b.our_y << ")"
     << (b.our_alive ? "" : " [已阵亡]") << "\n"
     << "我方得分: 资源=" << b.our_scores.resource_pts
     << " 对抗=" << b.our_scores.combat_pts
     << " 任务=" << b.our_scores.task_pts
     << " 总分=" << b.our_scores.total
     << "（资源满格=" << (b.our_scores.resource_saturated ? "是" : "否")
     << " 对抗满格=" << (b.our_scores.combat_saturated ? "是" : "否")
     << " 任务满格=" << (b.our_scores.task_saturated ? "是" : "否") << "）\n";
  for (const auto& o : b.opponents) {
    os << "对手 " << o.team << ": 最后位置=(" << o.last_x << "," << o.last_y
       << ") tick=" << o.last_seen_tick
       << " HP=" << (o.hp_known ? std::to_string(o.last_hp) : "?")
       << " 估计资源=" << o.est_r2
       << " 攻击倾向=" << o.aggression << " 置信度=" << o.confidence << "\n";
  }
  os << "事件数: " << b.events.size() << "\n";
  for (const auto& e : b.events) {
    os << "  事件: " << type_to_string(e.type)
       << " 阶段="
       << (derive_phase(e, b.tick) == EventPhase::Active
               ? "生效中"
               : (derive_phase(e, b.tick) == EventPhase::Expired ? "已结束"
                                                                 : "公告中"))
       << " 至 tick=" << e.until_tick << " damage=" << e.damage
       << " heal=" << e.heal;
    if (e.has_rect)
      os << " rect=[" << e.rect_x0 << "," << e.rect_y0 << "," << e.rect_x1
         << "," << e.rect_y1 << "]";
    if (!e.points.empty())
      os << " points=" << e.points.size() << "个";
    os << "\n";
  }
  if (!b.mission_text.empty()) os << "任务原文: " << b.mission_text << "\n";
  os << "\n=== 候选方案（本地 EV） ===\n";
  int idx = 0;
  for (const auto& g : goals) {
    os << "[" << idx++ << "] " << g.reason
       << " EV=" << g.ev << "（收益=" << g.expected_delta
       << " 风险折价=" << g.risk_penalty << "）\n";
  }
  os << "\n请输出 JSON。\n";
  return os.str();
}

// ---------------------------------------------------------------------------
// LLM 输出解析
// ---------------------------------------------------------------------------

bool parse_event_understanding(const std::string& json, EventState& out,
                               std::vector<std::string>& warnings) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(json);
  } catch (const std::exception&) {
    return false;
  }
  if (!j.is_object()) return false;
  const std::string where = "event_understanding";

  // type
  const auto it_type = j.find("type");
  if (it_type != j.end() && it_type->is_string()) {
    out.type = type_from_string(it_type->get<std::string>());
    if (out.type == EventType::Unknown)
      add_warning(warnings, where + ": 未知 type 字符串");
  }

  // region：rect 优先，兼容 [x0,y0,x1,y1] 与 [[x0,y0],[x1,y1]] 两种形态
  const auto it_region = j.find("region");
  if (it_region != j.end() && it_region->is_object()) {
    const auto it_rect = it_region->find("rect");
    if (it_rect != it_region->end() && it_rect->is_array()) {
      bool ok_all = true;
      long long x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      if (it_rect->size() == 4) {
        for (const auto& el : *it_rect)
          if (!el.is_number_integer()) ok_all = false;
        if (ok_all) {
          x0 = (*it_rect)[0].get<long long>();
          y0 = (*it_rect)[1].get<long long>();
          x1 = (*it_rect)[2].get<long long>();
          y1 = (*it_rect)[3].get<long long>();
        }
      } else if (it_rect->size() == 2 && (*it_rect)[0].is_array() &&
                 (*it_rect)[1].is_array() && (*it_rect)[0].size() == 2 &&
                 (*it_rect)[1].size() == 2 &&
                 (*it_rect)[0][0].is_number_integer() &&
                 (*it_rect)[0][1].is_number_integer() &&
                 (*it_rect)[1][0].is_number_integer() &&
                 (*it_rect)[1][1].is_number_integer()) {
        x0 = (*it_rect)[0][0].get<long long>();
        y0 = (*it_rect)[0][1].get<long long>();
        x1 = (*it_rect)[1][0].get<long long>();
        y1 = (*it_rect)[1][1].get<long long>();
      } else {
        ok_all = false;
      }
      if (ok_all && x0 >= 0 && y0 >= 0 && x1 >= 0 && y1 >= 0) {
        out.has_rect = true;
        out.rect_x0 = static_cast<int>(std::min(x0, x1));
        out.rect_y0 = static_cast<int>(std::min(y0, y1));
        out.rect_x1 = static_cast<int>(std::max(x0, x1));
        out.rect_y1 = static_cast<int>(std::max(y0, y1));
      } else {
        add_warning(warnings, where + ": rect 格式非法或含负坐标，忽略");
      }
    }
    const auto it_points = it_region->find("points");
    if (it_points != it_region->end() && it_points->is_array()) {
      for (const auto& pt : *it_points) {
        if (pt.is_array() && pt.size() == 2 && pt[0].is_number_integer() &&
            pt[1].is_number_integer()) {
          const long long x = pt[0].get<long long>();
          const long long y = pt[1].get<long long>();
          if (x >= 0 && y >= 0 && x <= 1000000 && y <= 1000000)
            out.points.push_back({static_cast<int>(x), static_cast<int>(y)});
          else
            add_warning(warnings, where + ": 点越界，忽略");
        } else {
          add_warning(warnings, where + ": 点格式非法，忽略");
        }
      }
    }
  }

  // numbers
  const auto it_num = j.find("numbers");
  if (it_num != j.end() && it_num->is_object()) {
    bool ok = false;
    out.damage = read_double(*it_num, "damage", 0.0, 0.0, 1e5, ok, warnings,
                             where + ".numbers");
    out.heal = read_double(*it_num, "heal", 0.0, 0.0, 1e5, ok, warnings,
                           where + ".numbers");
    out.duration = static_cast<int>(read_int(*it_num, "duration", 0, 0, 1000000,
                                             ok, warnings,
                                             where + ".numbers"));
  }

  // timing
  const auto it_timing = j.find("timing");
  if (it_timing != j.end() && it_timing->is_object()) {
    bool ok = false;
    out.active_tick = static_cast<int>(read_int(
        *it_timing, "active_tick", -1, -1, 2000000000LL, ok, warnings,
        where + ".timing"));
    out.until_tick = static_cast<int>(read_int(
        *it_timing, "until_tick", -1, -1, 2000000000LL, ok, warnings,
        where + ".timing"));
  }

  // reason
  const auto it_reason = j.find("reason");
  if (it_reason != j.end() && it_reason->is_string())
    out.reason = it_reason->get<std::string>();
  return true;
}

bool parse_weight_hints(const std::string& json, LlmWeightHints& out,
                        std::vector<std::string>& warnings) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(json);
  } catch (const std::exception&) {
    return false;
  }
  if (!j.is_object()) return false;
  const std::string where = "llm_weights";
  const auto it = j.find("weights");
  if (it == j.end() || !it->is_object()) {
    add_warning(warnings, where + ": 缺 weights 对象，无调整");
    return true;
  }
  bool ok = false;
  out.resource = read_double(*it, "resource", 1.0, 0.2, 5.0, ok, warnings,
                             where);
  out.has_resource = ok;
  ok = false;
  out.task = read_double(*it, "task", 1.0, 0.2, 5.0, ok, warnings, where);
  out.has_task = ok;
  ok = false;
  out.combat = read_double(*it, "combat", 1.0, 0.2, 5.0, ok, warnings, where);
  out.has_combat = ok;
  ok = false;
  out.survival = read_double(*it, "survival", 1.0, 0.2, 5.0, ok, warnings,
                             where);
  out.has_survival = ok;
  return true;
}

std::string hash_text(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;  // FNV offset basis
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;  // FNV prime
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(h));
  return std::string(buf);
}

}  // namespace rmtc_bot
