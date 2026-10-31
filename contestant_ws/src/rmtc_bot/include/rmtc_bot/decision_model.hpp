#pragma once
// 决策内核（离线纯函数层）：参数化计分模型、饱和度评估、信念层、
// 语义事件模型与目标效用比较。无 ROS/网络/LLM 依赖，可离线自检。
//
// 设计依据：docs/决策设计.md
//   - 不写死：所有规则数值经 DecisionParams 传入（默认值=当前规则标准配置，
//     仅离线兜底；比赛以开场规则/公告为准覆盖）；地图/资源/出生点现场学习；
//   - 规则红线（rules §8.1）：本模块绝不解析 event_text 自然语言本身——语义
//     事件模型只由 LLM 结构化输出构建（见 llm_advisor），本模块负责本地校验、
//     规划与执行层打分；事件数值可随场次变化，校验不得要求等于标准配置；
//   - 节奏从"饱和度信念"涌现（§1），不由 tick 日历决定。
//
// 解析原则沿用项目惯例：宽松读、严格用；字段问题逐项降级并记 warnings，
// 任何函数不抛异常。

#include <array>
#include <string>
#include <vector>

#include "rmtc_bot/perception.hpp"

namespace rmtc_bot {

// ---------------------------------------------------------------------------
// 参数化规则快照
// ---------------------------------------------------------------------------

// 当前规则标准配置的默认值（rules v1.2）：只作离线兜底与自检基准，
// 线上以 load_params_json 覆盖后的值为准。字段必须正数，否则保留默认并警告。
struct DecisionParams {
  int hp_max = 100;                    // 最大/初始生命
  int attack_damage = 20;              // 普攻对普通目标
  int attack_damage_defended = 10;     // 普攻对防御目标
  double death_penalty = 60.0;         // 每次死亡扣除的有效伤害
  double kill_reward = 60.0;           // 每名被击毁车辆的击杀奖励总量
  double resource_score_cap_value = 120.0;  // R2 满格阈值
  double resource_score_max = 30.0;         // 资源满分
  double combat_score_cap_d = 500.0;        // D 满格阈值
  double combat_score_max = 15.0;           // 对抗满分
  double task_score_each = 5.0;             // 单任务分
  int task_count_max = 3;                   // 任务数上限
  int respawn_wait_ticks = 3;               // 普通死亡复活等待
  int spawn_protect_ticks = 2;              // 出生保护
  int match_ticks = 500;                    // 场次总 tick
  int llm_quota = 100;                      // 每席位成功调用上限（10/30 官方公告确认 100 次/场；本队预算 30）
  // R5-G 巡逻实证覆盖率门槛（10/30 V8 实测）：踩过的环格占全环比例 ≥ 此值
  // 即视为"绕环满一圈"实证，允许资源侧参战锁存（默认 90%）。
  double patrol_lap_coverage = 0.9;
  // R5-P 追击纪律（10/31，用户："持续追击的 tick 要有限制，但能把对面压进
  // 角落就追到死"）：追击预算与止损。距离一律用障碍感知 BFS 距离（不是
  // Chebyshev——绕墙时直线距离骗人）。
  int pursue_max_ticks = 10;     // 单次追击最长 tick 数（封角/可击杀例外）
  int pursue_max_stall = 4;      // 连续这么多 tick 距离没缩短 → 放弃
  int pursue_cooldown = 6;       // 放弃后多少 tick 内不再发起追击（防来回拉扯）
  int pursue_corner_escape = 1;  // 目标可逃格 ≤ 此值 = 压进死角 → 追到死
};

// 从 JSON 配置加载并覆盖默认值。文件不可读/顶层非对象 → 返回 false（沿用默认并警告）；
// 可读但个别字段缺失/非法 → 该字段保留默认并警告，返回 true。
// 识别的键与 DecisionParams 字段同名（小写下划线风格两种都收：hp_max / hpMax）。
bool load_params_json(const std::string& path, DecisionParams& out,
                      std::vector<std::string>& warnings);

// ---------------------------------------------------------------------------
// 计分与饱和度
// ---------------------------------------------------------------------------

struct ScoreComponents {
  double resource_pts = 0.0;
  double combat_pts = 0.0;
  double task_pts = 0.0;
  double total = 0.0;
  bool resource_saturated = false;  // R2 达到满格阈值
  bool combat_saturated = false;    // D 达到满格阈值
  bool task_saturated = false;      // M 达到任务数上限
  double resource_margin = 0.0;     // 距资源满格还差多少 R2
  double combat_margin = 0.0;       // 距对抗满格还差多少 D
};

// 按计分公式计算各分项得分与饱和度（§1）。
ScoreComponents compute_scores(double r2, double d, int tasks_done,
                               const DecisionParams& p);

// 死亡成本（分）= death_penalty / combat_cap × combat_max ≈ 1.8
double death_cost_points(const DecisionParams& p);

// 击杀奖励价值（分）= kill_reward / combat_cap × combat_max ÷ 致死批次贡献人数。
// contributors<=0 按 1 处理。
double kill_reward_points(const DecisionParams& p, int contributors);

// ---------------------------------------------------------------------------
// 语义事件模型（只能由 LLM 结构化输出构建；本地只校验与规划）
// ---------------------------------------------------------------------------

enum class EventType {
  Unknown,      // 未识别/解析失败
  Explosion,    // 矩形环境伤害
  Illusion,     // 方向反转
  HealZone,     // 回血点
  Fortress,     // 堡垒增益
  WorldSplit,   // 十字临时墙 + 即死即活
  Chili,        // 辣椒火线
  Thorns,       // 反伤
  Vulnerable,   // 易伤
  Other,        // 目录外类型（保守处理）
};

enum class EventPhase {
  Announced,  // 已公告，未生效
  Active,     // 生效中
  Expired,    // 已结束
};

struct EventState {
  EventType type = EventType::Unknown;
  int announced_tick = -1;  // 首次进入信念的 tick
  int active_tick = -1;     // 生效 tick；-1 = 未知
  int until_tick = -1;      // 结束 tick（含）；-1 = 未知
  bool has_rect = false;
  int rect_x0 = 0, rect_y0 = 0, rect_x1 = 0, rect_y1 = 0;  // 含端点闭区间
  std::vector<std::array<int, 2>> points;                  // 多点点位
  double damage = 0.0;  // 环境伤害数值（LLM 提取，可随场次变化）
  double heal = 0.0;    // 回复数值（LLM 提取）
  int duration = 0;     // 持续 tick 数（LLM 提取）
  std::string source_hash;  // 公告文本 hash（缓存/判重键）
  std::string reason;       // LLM 给的说明文字（日志/技术报告用）
};

// 按当前 tick 推导事件阶段。
EventPhase derive_phase(const EventState& e, int current_tick);

// 本地校验 LLM 构建的事件参数（§4.1 合规边界）：
//   - rect 须在地图 [0,W)×[0,H) 内（含端点）；points 逐点校验（越界点剔除）；
//   - 时间窗在 [0, match_ticks] 范围内（active/until 语义非法则降级）；
//   - damage/heal/duration 有限、非负、不超过宽松上限（防回绕；不要求等于标准配置）；
// 返回 false 表示事件整体不可用（调用方应丢弃该事件）。
bool validate_event_state(EventState& e, int map_w, int map_h, int current_tick,
                          const DecisionParams& p,
                          std::vector<std::string>& warnings);

// ---------------------------------------------------------------------------
// 信念层
// ---------------------------------------------------------------------------

// 单个对手的估计（估计值 + 置信度；离开视野后保留但置信度衰减）。
struct OpponentBelief {
  std::string team;
  bool seen = false;
  int last_x = -1, last_y = -1;
  int last_seen_tick = -1;
  int last_hp = -1;        // 最近观察到的 HP
  bool hp_known = false;
  double est_r2 = 0.0;     // 观察到的采集累计（价值）
  double est_d = 0.0;      // 估计有效伤害（v0：观察到的我方受击贡献近似）
  int est_tasks = 0;       // 疑似已完成任务数（低置信，仅参考）
  double confidence = 0.0; // 0..1
  double aggression = 0.0; // 0..1 攻击倾向滑动估计
  int defends_observed = 0;
};

struct MapMemory {
  int width = 0, height = 0;  // 0 = 未知
  std::vector<std::array<int, 2>> obstacles;      // 静态障碍（去重）
  struct Resource {
    int x = 0, y = 0;
    int value = 0;
  };
  std::vector<Resource> resources_known;          // 已知未采集资源
  std::vector<std::array<int, 2>> spawn_points;   // 观察到的复活/开局点
};

struct Belief {
  int tick = -1;
  // 我方状态镜像（来自感知）
  int our_x = -1, our_y = -1;
  int our_hp = -1;
  bool our_hp_known = false;
  bool our_alive = true;
  bool our_defending = false;
  bool our_protected = false;
  ScoreComponents our_scores;
  std::vector<Mission> missions;  // 结构化任务
  std::vector<std::string> completed_mission_ids;  // 已完成任务 id
  std::string mission_text;       // 任务原文（LLM 解析用）
  std::string event_text;         // 当前公告原文（LLM 解析用，本地不解析）
  std::vector<OpponentBelief> opponents;
  MapMemory map;
  std::vector<EventState> events;

  // 跨帧状态（update_belief 内部使用）
  std::vector<std::array<int, 3>> prev_opp_pos;  // {对手下标,x,y}
  int prev_our_hp = -1;
};

// 用一帧感知更新信念。首帧初始化地图；跨帧做资源消失归因、对手 HP/倾向跟踪。
// 纯函数式累积，不抛异常；问题记 warnings。
void update_belief(Belief& b, const Perception& p, const DecisionParams& params,
                   std::vector<std::string>& warnings);

// ---------------------------------------------------------------------------
// 目标效用（候选生成 + EV）
// ---------------------------------------------------------------------------

enum class GoalType {
  Collect,     // 采集已知资源
  Engage,      // 接战指定对手
  Retreat,     // 后撤避险
  DefendHold,  // 原地防御抗集火
  GotoTask,    // 前往任务点（reach）
  EventStance, // 事件立场（Exploit 生成此候选；Avoid 以约束形式并入其它候选）
};

struct GoalCandidate {
  GoalType type = GoalType::Collect;
  std::string target_team;    // Engage
  std::string event_hash;     // EventStance
  int x = -1, y = -1;         // 目标坐标；-1 = 无具体坐标
  double expected_delta = 0.0;   // 期望分增量（正收益项合计）
  double risk_penalty = 0.0;     // 期望死亡折价（负向）
  double ev = 0.0;               // 净期望值 = expected_delta - risk_penalty
  std::string reason;
};

// 决策权重（LLM 可给出乘性调整建议，见 llm_advisor::LlmWeightHints）。
struct DecisionWeights {
  double resource = 1.0;
  double task = 1.0;
  double combat = 1.0;
  double survival = 1.0;  // 死亡风险折价的放大系数
};

// LLM 权重调整建议（乘性，限幅 [0.2, 5.0]；本地校验后应用）。
struct LlmWeightHints {
  bool has_resource = false;
  double resource = 1.0;
  bool has_task = false;
  double task = 1.0;
  bool has_combat = false;
  double combat = 1.0;
  bool has_survival = false;
  double survival = 1.0;
};

// 应用权重建议：逐字段校验域，越界字段拒收并警告；结果再整体限幅防溢出。
void apply_llm_weight_hints(DecisionWeights& w, const LlmWeightHints& h,
                            std::vector<std::string>& warnings);

// 生成候选目标并本地打分（确定性、无 LLM）。EV 公式全参数化，见 docs/决策设计.md §3。
std::vector<GoalCandidate> generate_goal_candidates(
    const Belief& b, const DecisionParams& params, const DecisionWeights& w,
    std::vector<std::string>& warnings);

// 取 EV 最大候选；平局保持候选集顺序（Collect 先生成，平局偏保守）。
// 全部 EV<=0 时返回 nullptr（执行层走安全兜底）。
const GoalCandidate* pick_best_goal(const std::vector<GoalCandidate>& goals);

// ---------------------------------------------------------------------------
// 事件立场（饱和度指导事件处理，docs/决策设计.md §4.3）
// ---------------------------------------------------------------------------

enum class EventStance {
  Avoid,    // 回避（含 Unknown/解析失败，一律保守）
  Exploit,  // 主动利用
  Neutral,  // 无强立场，由执行层参考事件表补偿
};

struct StanceDecision {
  EventStance stance = EventStance::Avoid;
  std::string reason;
};

// v0 立场规则：数值来自 LLM 提取的参数，不写死。
StanceDecision event_stance(const EventState& e, const Belief& b,
                            const DecisionParams& p,
                            std::vector<std::string>& warnings);

}  // namespace rmtc_bot
