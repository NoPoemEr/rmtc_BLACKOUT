#pragma once
// 策略接口与工厂（collect / combat 两档已实现并登记）
//
// 职责分工（先读清楚再动手）：
//   - bot_node 保证传给 decide() 的 Perception：解析成功、是当前场次（session 已对齐）、
//     且 tick 严格大于上一帧（旧/重复帧已被丢弃）；
//   - Strategy 只做纯计算：读感知 + 最近一次 LLM 结论，返回本 Tick 的一个动作；
//     禁止阻塞（不准 sleep、不准同步调 LLM），否则会错过动作窗口；
//   - 换场时 bot_node 调用 on_new_session()，在这里清空你的地图记忆/规划缓存；
//   - 动作消息的 tick / session_id 由 bot_node 原样回填，策略不用管。
//
// 协议相关事实（决定接口长这样）：
//   - 每 Tick 最多执行一个动作；动作语法：move/attack + direction(up/down/left/right)、
//     defend、wait（小写字段）；
//   - 同 Tick 可重复提交，最后一条"格式合规"的候选覆盖之前；坏格式的不覆盖。
//     因此我们每 Tick 只发一次、发就发合规的；
//   - 恢复时可能重发相同 Tick：decide() 对同一帧应给出同样的动作，
//     且内部记忆更新要按 tick 去重（别把一步记成两步）。

#include <memory>
#include <string>

#include "rmtc_bot/llm_client.hpp"
#include "rmtc_bot/perception.hpp"

namespace rmtc_bot {

// 决策输出：只含动作字段 + 一行理由
struct ActionChoice {
  std::string action;     // "move" / "attack" / "defend" / "wait"
  std::string direction;  // "up" / "down" / "left" / "right"（仅 move/attack 需要）
  std::string reason;     // 决策理由：只进日志（检录截图素材），不进动作消息

  // 便捷构造：不用每次手写字段
  static ActionChoice Wait(const std::string &reason) {
    return ActionChoice{"wait", "", reason};
  }
  static ActionChoice Move(const std::string &dir, const std::string &reason) {
    return ActionChoice{"move", dir, reason};
  }
  static ActionChoice Attack(const std::string &dir, const std::string &reason) {
    return ActionChoice{"attack", dir, reason};
  }
  static ActionChoice Defend(const std::string &reason) {
    return ActionChoice{"defend", "", reason};
  }
};

// 策略基类：每 Tick 调用一次 decide()
class Strategy {
 public:
  virtual ~Strategy() = default;

  // p：当前帧感知（已通过协议校验）。
  // llm：最近一次完成的 LLM 调用结论。可能是 nullptr（客户端未配置或从未完成过）；
  //   即使非空也可能是失败（超时/HTTP 错误）——用之前先看 llm->status == LlmResult::Status::SUCCESS。
  virtual ActionChoice decide(const Perception &p, const LlmResult *llm) = 0;

  // 新场次（session_id 变化）回调：清空全部记忆/规划/动作缓存
  virtual void on_new_session(const std::string &session_id) { (void)session_id; }

  // 策略档位名（日志/自检用）
  virtual const char *name() const = 0;
};

// 占位策略：永远 wait。M1/M2 联调默认档，保证通信链路可独立验收。
class WaitStrategy : public Strategy {
 public:
  ActionChoice decide(const Perception &p, const LlmResult *llm) override {
    (void)p;
    (void)llm;
    return ActionChoice::Wait("占位档：wait");
  }
  const char *name() const override { return "wait"; }
};

// 按 decision_config.json 的 decision_type 创建策略（实现见 strategy.cpp 顶部说明）。
std::unique_ptr<Strategy> make_strategy();

}  // namespace rmtc_bot
