// combat_strategy.cpp —— 第二阶段对抗策略（10/27 MVP 均衡规则层 + 10/28 v2 决策层接入与优化）
//
// decide() 漏斗 v2（硬 → 软，逐层降级；门槛全部由 DecisionParams 推导，无规则数值字面量）：
//   0. 同帧幂等缓存（恢复重发同 Tick 输出逐字节一致，selftest 10）
//   1. 信念更新（decision_model::update_belief：障碍记忆/饱和度/出生点）
//   2. 硬红线：阵亡 → wait；无位置/地图 → wait（selftest 1/2）
//   3. 攻击判定（selftest 4–8 + 14/15 + 26/27）：
//      - 直线 1–2 格最近敌车，目标非保护、我方非保护期（V7/R4-A：防御中
//        目标照打——半伤落井下石，其本 Tick 不反击）；
//      - 方向内不得有更近的敌车遮挡（打了也命中近车，白浪费 Tick）；
//      - 两遍扫描：可击杀（hp ≤ attack_damage）优先于"有利可图"；
//      - R5-N（10/30）：取消"对抗分饱和只打可击杀"——饱和后照常按未饱和
//        策略换血/追猎（超出 500 的 D 自己不加分，但每次击杀扣对手 60 D、
//        掐对手资源，排名是相对的；死亡代价已由 R1 生存门 + favorable/
//        safe_trade 定价，selftest 78/79）；
//      - 低血（< 2×attack_damage）第二遍只打严格领先一枪的目标（V6：不再
//        整块跳过换血判定——低血不躺平，但必输换血不送头）；
//      - R2（10/28 自对弈 round2 对称换血）：保留 IsIsolated 换血（官方陪练
//        三轮回归证明攻击倾向门/一刀切都掉伤害掉排名，见 round2 分析）；镜像
//        拖血到 20/20 的死锁改由 R1 生存门 + R2-B 回血路线兜底。
//      - R3-A（10/28 晚，用户拍板"该激进就大胆，主动寻求换血"）：第二遍换血
//        门槛②放宽为同血即打（原严格领先一枪）；落后仍不打；游走兜底满血时
//        单敌追猎（selftest 37–39；V7 起追猎挂参战开关）。
//      - V7/R4-A（10/30，用户拍板"人多聚堆就上前劝架、防御目标落井下石"）：
//        主动找架统一挂参战信念门——存在未完成 effective_damage 战斗任务，
//        或（任务全清且资源饱和）才主动参战；多敌聚堆改走集火参与（最低血
//        目标逼近 + 低血/R1 生存门/饱和只收头/枪线安全门兜底，selftest
//        47–56）。
//      - V8/R5-A~F（10/30 V7 首场练习赛实测 + 用户拍板，selftest 47b/
//        48c/57–67）：R5-A 参战门补"资源采尽"（已知资源全消失且视野无资源，
//        实测饱和线 120 在全图资源 200 的场次永远不可达 → 0 集火 53 避让）；
//        R5-B 幻觉自适应（回执方向反于发出方向即幻觉，emit 反向 move，
//        attack 不反，实测 tick 45–53 漂移贴边）；R5-C 障碍遗忘（WorldSplit
//        临时墙消失后不再挡 BFS，实测绕鬼墙 12 帧闭环徘徊 116 tick）；R5-D
//        巡逻仅到达路点才推进（绕行途中不被环格劫持锚定）；R5-E 敢死队
//        （本命赚 ≥80 回本后放宽 R1/低血门、不逃命，主动换伤害满血复活）；
//        R5-F 参战锁存（绕环满一圈才有"采尽"实证，锁存后偶见资源顺手捡、
//        不把参战门关回去——角落可能藏资源，但不确定的地图不能反向拖垮参战）；
//        R5-G（10/30 V8 练习赛实测，selftest 47b/66–69）：顺序指针满圈被多敌
//        避让反复劫持（实测 tick 470+ 才凑满、集火参与仅 1 次，而踩格覆盖
//        tick 317 已 28/28）→ 实证改踩格覆盖率 ≥ patrol_lap_coverage（默认
//        90%）；多敌避让在环上优先沿环逃逸，不再与巡逻互相拉拽振荡；
//        R5-H（10/30 V8.1 用户反馈"血量明显比对方高却不敢打"，selftest 70）：
//        参战门补血量碾压信号（≥3×attack_damage 且 ≥2×最低血敌车、≥最高血
//        敌车 → 集火最低血/单敌追猎）；
//        R5-I（10/30 V8.1 用户反馈"巡逻横穿地图中间"，selftest 71/72）：
//        环上沿环推进不 BFS 切腹地；位置瞬移 >1 格视同重生就近归环。
//      - V8.2/R5-J（10/30 用户："血量比对面高就应该追着打了，没必要做那么高
//        的限制"，selftest 73）：hp_dominant 砍掉 3×damage 下限与 2×最低血
//        条件，改为"我方血 > 视野内最强敌车"即参战；R4-A 逼近门槛改为血高于
//        目标（原 ≥2×damage），R3-A 追猎门槛改为血不低于对方（原 ≥3×damage）。
//        R1 生存门/威胁步进安全门保留——血高不等于送死（饱和度门在 R5-N 撤掉）。
//      - V8.2/R5-K（10/30 用户："环形巡逻轨迹就做正方形吧，不需要圆角"）：
//        BuildPatrolRing 去掉四角切角，改直角矩形环（上→右→下→左顺时针），
//        尺寸仍由地图 w/h 推导、非写死；极小图回退中心游走不变。
//      - V8.2/R5-L（10/30 用户："我们还有多敌避让吗？不是尽可能参战集火吗；
//        血量低并且赚够分了那就送一波"）：删除 R2-C"多敌聚集避让"整段——
//        多敌不再逃跑，血量压过最低血目标或本命回本（life_paid）就 R4-A 集火，
//        否则照常巡逻/采集；低血+未回本的避战仍由 5) 低血避火 兜底（selftest 74/75）。
//      - V8.2/R5-M（10/30 用户："在交火范围了又往前走一步让别人白打一下"）：
//        非参战巡逻/兜底不走入、不留在敌车直线射程（1–2 格），已在射程内
//        优先一步走出（selftest 76/77）。
//      - V8.2/R5-N（10/30 用户："超过 500 其实还是有比较空间的"）：饱和度不再
//        当"不还手/不追"的理由——V8.2 实测 A 520 / C 654，C 只差 2.5 分就翻盘，
//        超 500 的击杀（对手 -60 D）和掐资源在相对排名里仍然值钱；饱和后
//        攻击判定/R4-A 集火/R3-A 单敌追猎全部回到"未饱和"策略（selftest 78/79）。
//      - V8.3/R5-O（10/30 撤饱和门那一场实测）：A 1370 D / 13 杀 / 12 死，45.0 第一；
//        同分比的是**未截断**调整后有效伤害（rules v1.2 §10.2），C、D 同为 42.5 靠
//        830 vs 710 分出二三名——超 500 的伤害/击杀奖励（每杀 +60）真金白银。
//        被命中 56 次里 55 次我们同时开了火（V8.2 有 13 次"敌在射程内却没开炮"）；
//        唯一漏网的是 tick 29 低血避火必死局 → 新增 4b 必死还手（selftest 80/81）。
//      - V8.6（10/31 用户："最重要的是资源，哪怕为此掉血，只要不死也要多捡" /
//        "超过 500 为什么就不追人"）：资源未满且敌不在 2 格内 → 资源优先不主动
//        开追（selftest 88–89）；但资源优先只按资源判断，绝不按对抗分饱和判断——
//        R5-N 仍有效，超 500 照常集火/追猎（同分比较用未截断 D、击杀扣对手
//        60 D，selftest 92）；资源分饱和且伤害未满 → 停采不涨分资源，把 Tick
//        还给任务/巡逻/战斗（selftest 91）。V8.6c（10/31 晚用户复核）撤销了
//        V8.6 误加的 combat_capped（饱和停追），资源优先门只看资源、不看 D。
//   4. 防御保命（selftest 17/21）：直线火力不防御必死、防御减半可活 → defend；
//      低血被围且无合法撤离格 → defend（保守）
//   4b. 必死还手（V8.3 实测后新增，selftest 80/81）：连防御都活不下来时
//      （k_in×attack_damage_defended ≥ self_hp）不再移动/防御白挨——移动躲不掉
//      本 Tick 伤害，反伤损血又拿不到伤害分，索性打出这一炮（可收头优先）
//   5. 低血避火（V6，10/29 用户定调"低血不能见人就跑、天使等不到"）：低血
//      不再有跨 Tick 撤离态；仅在真实威胁（枪线下 k_in≥1 或 ≥2 车近身）时
//      撤离评分避让（P1/P2），威胁解除立即恢复得分活动；任务/采集/巡逻步进
//      有"不增加枪线威胁"安全门；R2-B 回血路线保留（天使恩泽公告时优先）
//   6. 采集：委托组合的 CollectStrategy（B1 方案 b；含撞墙学障碍/卡住换目标）
//   7. 任务点：reach 类 mission 用信念障碍 BFS 首步推进（B4 顺路）；我方资源分
//      已饱和 → 任务优先于采集（selftest 19）
//   8. 游走兜底：环形巡逻（10/29 V5，用户拍板"金边银肚皮草角"；V6 修复脱环
//      吸附；V8.2 R5-K 改直角矩形环）——距地图边缘 D 格的方环顺时针巡逻（上→
//      右→下→左），尺寸由地图 w/h 推导；脱环（采集/避火）保持环序推进，仅
//      重生时就近归环；环不可用回退旧中心游走；被挡方向轮换（selftest
//      13/20/41/42）；主动找架：V7 参战信念门开、或血量压过最低血目标、或本命
//      回本（life_paid）→ R4-A 集火最低血；占不到便宜就照常巡逻/采集，不再多敌
//      逃跑（V8.2 R5-L 删除 R2-C 避让，selftest 35/48/66/69/70/74/75）；恰 1 敌
//      且参战开 → R3-A 追猎（selftest 35–39 + 47–56）；V8.2 R5-J：血量高于视野
//      内最强敌车即参战（selftest 70/73）；R5-M：非参战巡逻不进敌车直线射程、
//      已在射程内优先走出（selftest 76/77）；R5-N：对抗分饱和后照常换血/追猎
//      （selftest 78/79）。
//
// 设计依据：docs/combat_design.md（§2 漏斗）、docs/决策设计.md（§1 计分与饱和度、
// §2 信念层、§3 战术表）。随机事件立场按 docs/决策设计.md §6 步骤 3 接线：
// llm 参数（llm_brain 最近一次成功结论）经 parse_conclusion + validate_conclusion
// 本地校验后，只用其 event_plan（avoid=false 增益点位）做低血回血路线；
// event_text 原文绝不本地解析（rules §8.1）。

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rmtc_bot/combat_strategy.hpp"
#include "rmtc_bot/llm_conclusion.hpp"

namespace rmtc_bot {
namespace {

using Cell = std::pair<int, int>;

// 活的敌车：robot 类型、非本队、有绝对坐标、hp>0（hp=0 表示阵亡车）
bool IsEnemy(const Entity &e, const Perception &p) {
  return e.type == "robot" && e.team != p.self.team && e.has_absolute &&
         e.has_hp && e.hp > 0;
}

// 目标孤立：周围 Chebyshev 距离 ≤2 无其他活的敌车（"白捡"条件之一）
bool IsIsolated(const Entity &target, const Perception &p) {
  for (const auto &e : p.entities) {
    if (&e == &target) continue;
    if (!IsEnemy(e, p)) continue;
    if (std::abs(e.abs_x - target.abs_x) <= 2 &&
        std::abs(e.abs_y - target.abs_y) <= 2) {
      return false;
    }
  }
  return true;
}

// (ex,ey) 能否攻击到 (cx,cy)：同行/同列且直线距离 1–2（攻击按移动前坐标结算）
bool CanHit(int ex, int ey, int cx, int cy) {
  const int dx = std::abs(ex - cx);
  const int dy = std::abs(ey - cy);
  return (dx == 0 && dy >= 1 && dy <= 2) || (dy == 0 && dx >= 1 && dx <= 2);
}

// (ex,ey) 的枪线是否经过 (cx,cy)：同行/同列且直线距离 1–4
// （P2：3–4 格外的敌车枪线也算威胁，撤离格评分用；攻击实伤仍用 CanHit）
bool InGunLine(int ex, int ey, int cx, int cy) {
  const int dx = std::abs(ex - cx);
  const int dy = std::abs(ey - cy);
  return (dx == 0 && dy >= 1 && dy <= 4) || (dy == 0 && dx >= 1 && dx <= 4);
}

// 方向轮换（游走被挡时用）：up → right → down → left → up
std::string RotateDir(const std::string &d) {
  if (d == "up") return "right";
  if (d == "right") return "down";
  if (d == "down") return "left";
  return "up";
}

// R5-B 幻觉（Illusion 方向反转事件）翻转：意图方向 → 实际发出方向
std::string FlipDir(const std::string &d) {
  if (d == "up") return "down";
  if (d == "down") return "up";
  if (d == "left") return "right";
  if (d == "right") return "left";
  return d;
}

// BFS 首步方向：从 (sx,sy) 到 (tx,ty) 的最短路径第一步；已在目标点/不可达 → ""
// （与 collect_strategy 同算法形态；障碍取信念记忆，4 邻域 up/down/left/right）
std::string BfsFirstStep(int sx, int sy, int tx, int ty, int w, int h,
                         const std::set<Cell> &blocked) {
  if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || tx < 0 || ty < 0 ||
      sx >= w || sy >= h || tx >= w || ty >= h) {
    return "";
  }
  if (sx == tx && sy == ty) return "";
  std::vector<std::vector<int>> dist(h, std::vector<int>(w, -1));
  std::vector<std::vector<Cell>> prev(h, std::vector<Cell>(w, {-1, -1}));
  const int dx[4] = {0, 0, -1, 1};
  const int dy[4] = {-1, 1, 0, 0};
  std::queue<Cell> q;
  dist[sy][sx] = 0;
  q.push({sx, sy});
  while (!q.empty()) {
    Cell c = q.front();
    q.pop();
    if (c.first == tx && c.second == ty) break;
    for (int i = 0; i < 4; ++i) {
      int nx = c.first + dx[i];
      int ny = c.second + dy[i];
      if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      if (blocked.count({nx, ny})) continue;
      if (dist[ny][nx] != -1) continue;
      dist[ny][nx] = dist[c.second][c.first] + 1;
      prev[ny][nx] = c;
      q.push({nx, ny});
    }
  }
  if (dist[ty][tx] < 0) return "";
  Cell step = {tx, ty};
  while (prev[step.second][step.first] != Cell(-1, -1) &&
         prev[step.second][step.first] != Cell(sx, sy)) {
    step = prev[step.second][step.first];
  }
  if (step.first == sx + 1 && step.second == sy) return "right";
  if (step.first == sx - 1 && step.second == sy) return "left";
  if (step.first == sx && step.second == sy + 1) return "down";
  if (step.first == sx && step.second == sy - 1) return "up";
  return "";
}

// BFS 最短路径长度（4 邻域，障碍取信念记忆）；不可达 → -1。
// R5-P（10/31）追击纪律用：绕墙时 Chebyshev 距离会骗人（明明隔一堵墙 2 格，
// 实际要绕 8 步），追击预算/停滞判定必须按真实步数算。
int BfsDist(int sx, int sy, int tx, int ty, int w, int h,
            const std::set<Cell> &blocked) {
  if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || tx < 0 || ty < 0 ||
      sx >= w || sy >= h || tx >= w || ty >= h) {
    return -1;
  }
  if (sx == tx && sy == ty) return 0;
  std::vector<std::vector<int>> dist(h, std::vector<int>(w, -1));
  const int dx[4] = {0, 0, -1, 1};
  const int dy[4] = {-1, 1, 0, 0};
  std::queue<Cell> q;
  dist[sy][sx] = 0;
  q.push({sx, sy});
  while (!q.empty()) {
    Cell c = q.front();
    q.pop();
    for (int i = 0; i < 4; ++i) {
      int nx = c.first + dx[i];
      int ny = c.second + dy[i];
      if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      if (blocked.count({nx, ny})) continue;
      if (dist[ny][nx] != -1) continue;
      dist[ny][nx] = dist[c.second][c.first] + 1;
      if (nx == tx && ny == ty) return dist[ny][nx];
      q.push({nx, ny});
    }
  }
  return -1;
}

// 目标可逃格数：4 邻域里能走的格子数（界内、非障碍、非车占位）。
// R5-Q（10/31，"能把对面压进角落就追到死"）：≤1 = 压进死角/单口，
// 此时追击不再受预算限制，追到死；否则按 R5-P 的预算止损。
int EscapeCells(int tx, int ty, int w, int h, const std::set<Cell> &blocked) {
  const int nx[4] = {tx, tx, tx - 1, tx + 1};
  const int ny[4] = {ty - 1, ty + 1, ty, ty};
  int n = 0;
  for (int i = 0; i < 4; ++i) {
    if (nx[i] < 0 || ny[i] < 0 || nx[i] >= w || ny[i] >= h) continue;
    if (blocked.count({nx[i], ny[i]})) continue;
    ++n;
  }
  return n;
}

// 环形巡逻路径（10/29 V5，用户拍板"金边银肚皮草角"）：距地图边缘 D 格的
// 圆角方形环，顺时针闭合。四角切角——环角格 (D,D) 等被跳过，切角格比环角
// 离地图角（出生/复活点危险区）远 1–2 格；环中间段距边缘 D 格，既不贴边
// 也不进中心热点区（天下无双堡垒等）。极小图放不下环（需 W,H ≥ 2D+3）时
// 返回空 → 调用方回退旧中心游走。
std::vector<Cell> BuildPatrolRing(int w, int h) {
  std::vector<Cell> ring;
  if (w < 7 || h < 7) return ring;  // 太小：放不下任何环
  int d = std::min(5, std::max(1, (std::min(w, h) - 1) / 2 - 1));
  while (d >= 1 && (w < 2 * d + 3 || h < 2 * d + 3)) --d;
  if (d < 1) return ring;
  const int left = d, right = w - 1 - d, top = d, bottom = h - 1 - d;
  // R5-K（10/30 用户拍板"巡逻走正方形"）：直角矩形环，去掉 V5 的四角切角。
  // 顺时针一圈：上边 → 右边 → 下边 → 左边。尺寸完全由地图 w/h 推导
  // （非写死）：地图是方是矩都按比例取距边 d 的环；极小图回退中心游走。
  for (int x = left; x <= right; ++x) ring.push_back({x, top});
  for (int y = top + 1; y <= bottom; ++y) ring.push_back({right, y});
  for (int x = right - 1; x >= left; --x) ring.push_back({x, bottom});
  for (int y = bottom - 1; y >= top + 1; --y) ring.push_back({left, y});
  return ring;
}

}  // namespace

CombatStrategy::CombatStrategy() : collector_(/*explore_enabled=*/false) {
  // 可选决策参数：从 RMTC_DECISION_CONFIG / config/decision_config.json 顶层
  // 数值键加载（无键/文件缺失 → 规则标准配置默认值；见 decision_model.hpp）
  const char *env = std::getenv("RMTC_DECISION_CONFIG");
  const std::string path =
      (env != nullptr && env[0] != '\0') ? env : "config/decision_config.json";
  std::vector<std::string> warnings;
  load_params_json(path, params_, warnings);
  for (const auto &w : warnings) std::fprintf(stderr, "[combat] %s\n", w.c_str());
}

ActionChoice CombatStrategy::decide(const Perception &p, const LlmResult *llm) {
  // R5-B 幻觉检测（10/30 V8 练习赛实测 tick 45–53：Illusion 方向反转下我们
  // 发 right 实走 left，一路漂到地图边缘贴边被打）：上一 Tick move 回执方向
  // 与我们实际发出的方向相反 → 幻觉生效（此后 move 反向发出，见 emit）；
  // 一致 → 解除；未移动/非 move → 状态保持。放在幂等缓存之前（检测幂等）。
  if (p.self.has_last_action && p.self.last_action == "move" &&
      p.self.last_moved && p.self.has_last_direction &&
      !last_emitted_dir_.empty()) {
    const std::string &got = p.self.last_effective_direction;
    if (got == FlipDir(last_emitted_dir_)) {
      illusion_ = true;
    } else if (got == last_emitted_dir_) {
      illusion_ = false;
    }
  }
  // R6-B（10/31 第二阶段 v90 实测 tick 57–63）：幻觉补偿自愈。补偿生效时我们
  // 发出的是反向意图，裁判执行后应落在我们算过的合法格；一旦 move 回执明确
  // 报 E_OOB（无方向），说明反转已结束、或贴边时把合法意图翻成了越界方向，
  // 继续反向发只会每 Tick 空转（实测连续 7 Tick，且中间夹攻击段没有移动回执，
  // 走不了上面的"方向比对"解除）。据此立即停止补偿，下一 Tick 用回执重新判定：
  // 若反转仍在，回执会再次呈现反向，自会重新开启补偿。
  if (illusion_ && p.self.has_last_action && p.self.last_action == "move" &&
      !p.self.last_moved && p.self.has_last_error &&
      p.self.last_error == "E_OOB") {
    illusion_ = false;
  }
  // 0) 同帧幂等（恢复重发同 Tick 输出逐字节一致）
  if (has_cached_ && p.tick == last_tick_) return cached_;
  has_cached_ = true;
  last_tick_ = p.tick;
  auto emit = [this](ActionChoice c) {
    // R5-B：幻觉生效时 move 意图方向整体反向（attack/defend 不受幻觉影响，
    // 10/30 实测攻击方向照常）——裁判执行时会反转，我们反着发才能走对；
    // 内部所有路径计算（BFS/枪线/巡逻）保持真实空间。
    if (c.action == "move") {
      if (illusion_) c.direction = FlipDir(c.direction);
      last_emitted_dir_ = c.direction;  // 实际发出方向（与回执比对）
    }
    cached_ = c;
    return c;
  };

  // 1) 信念更新（每 Tick 一次；换场由 on_new_session 清空）
  {
    std::vector<std::string> warnings;
    update_belief(belief_, p, params_, warnings);
    for (const auto &w : warnings) {
      std::fprintf(stderr, "[combat] belief: %s\n", w.c_str());
    }
  }

  // 1.5) LLM 结论（R2-B 接线，docs/决策设计.md §6 步骤 3）：llm_brain 最近一次
  //   成功结论 → parse_conclusion 结构化 + validate_conclusion 本地校验（坐标
  //   裁剪/时效/白名单）；解析失败沿用上一次有效结论（llm_prompt_design §4）。
  //   本层绝不解析 event_text 原文（rules §8.1）；结论来自官方 AI 服务返回。
  if (llm != nullptr && llm->status == LlmResult::Status::SUCCESS) {
    std::vector<std::string> warnings;
    LlmConclusion fresh;
    if (parse_conclusion(llm->content, fresh, warnings)) {
      // 白名单 = 任务类型 ∪ 任务 id：实测 deepseek-flash 单消息模式下偶尔
      // 回 id 值（如 collect_two_normal）而非 type；id 也放行（下游只当
      // "结论非空"信号用，不按类型消费），避免整条 goals 被误清空。
      std::vector<std::string> allowed;
      for (const auto &m : p.missions) {
        allowed.push_back(m.type);
        allowed.push_back(m.id);
      }
      validate_conclusion(fresh, p.map_width, p.map_height, p.tick, allowed,
                          warnings);
      last_conclusion_ = fresh;
      has_conclusion_ = true;
    }
    for (const auto &w : warnings) {
      std::fprintf(stderr, "[combat] llm: %s\n", w.c_str());
    }
  }

  // 2) 硬红线
  if (!p.self.has_alive || !p.self.alive) {
    alive_prev_ = false;
    return emit(ActionChoice::Wait("阵亡：等待复活"));
  }
  // V6（10/29）：重生检测——环形巡逻只在真正复活时就近归环
  const bool just_respawned = !alive_prev_;
  alive_prev_ = true;
  if (!p.self.has_position || p.map_width <= 0 || p.map_height <= 0) {
    return emit(ActionChoice::Wait("无有效位置或地图信息"));
  }

  const int x = p.self.x;
  const int y = p.self.y;
  // R5-I（10/30 V8.1 实测：tick 135 死 → tick 136 直接满血出现在出生点
  // [1,1]，中间没有 alive=false 帧 → just_respawned 不触发、重生就近归环
  // 失效、BFS 从角落直插对岸路点横穿地图中间）：位置瞬移 >1 格视同重生。
  const bool teleported =
      last_x_ >= 0 &&
      (std::abs(x - last_x_) > 1 || std::abs(y - last_y_) > 1);
  last_x_ = x;
  last_y_ = y;
  // R5-E 敢死队（10/30 V8，用户拍板"残血不用非要避战"）：本命回本判定——
  // 复活帧记录净伤害基线（裁判死亡时已扣 60），本命累计 ≥ death_penalty +
  // attack_damage（80）即"回本"：这条命赚够了下一条命的罚款，死得起。
  if (just_respawned || teleported || !life_baseline_set_) {
    life_baseline_ = p.score.effective_damage;
    life_baseline_set_ = true;
  }
  const bool life_paid =
      life_baseline_set_ &&
      (p.score.effective_damage - life_baseline_) >=
          (params_.death_penalty + params_.attack_damage);
  const int self_hp = p.self.has_hp ? p.self.hp : 100;
  const int w = p.map_width;
  const int h = p.map_height;

  // 已知障碍 + 敌车占用格（BFS/撤离共用；信念障碍来自视野累积）
  std::set<Cell> blocked;
  for (const auto &o : belief_.map.obstacles) blocked.insert({o[0], o[1]});
  for (const auto &e : p.entities) {
    if (IsEnemy(e, p)) blocked.insert({e.abs_x, e.abs_y});
  }
  const auto cell_blocked = [&](int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= w || cy >= h) return true;
    return blocked.count({cx, cy}) != 0;
  };

  // R5-P 追击纪律（10/31，用户："持续追击的 tick 要有限制，但能把对面压进
  // 角落就追到死"）：距离一律用障碍感知 BFS 步数（绕墙时直线距离会骗人：
  // 隔一堵墙看着 2 格，实际要绕 8 步）；封角/贴身不收预算限制，其余按
  // tick 预算 + 停滞止损 + 冷却叫停，把时间还给巡逻/采集/任务。
  if (pursue_cool_ > 0) --pursue_cool_;
  // 上一次决策不是追击 → 本次追击预算重新起算；连续追击（哪怕换目标）累计。
  if (!last_was_pursuit_) {
    pursue_key_x_ = -1;
    pursue_key_y_ = -1;
    pursue_ticks_ = 0;
    pursue_stall_ = 0;
    pursue_best_d_ = -1;
  }
  last_was_pursuit_ = false;
  const auto pursuit_dist = [&](int fx, int fy, const Entity &t) {
    std::set<Cell> b = blocked;
    b.erase({t.abs_x, t.abs_y});  // 目标自身格不算障碍（要走到它旁边）
    return BfsDist(fx, fy, t.abs_x, t.abs_y, w, h, b);
  };
  const auto target_escape = [&](const Entity &t) {
    std::set<Cell> b = blocked;
    b.insert({x, y});             // 我方占位也算封锁（堵口）
    b.erase({t.abs_x, t.abs_y});
    return EscapeCells(t.abs_x, t.abs_y, w, h, b);
  };
  const auto escape_after = [&](int nx, int ny, const Entity &t) {
    std::set<Cell> b = blocked;
    b.insert({nx, ny});           // 走到该格后目标还剩几个逃生口
    b.erase({t.abs_x, t.abs_y});
    return EscapeCells(t.abs_x, t.abs_y, w, h, b);
  };
  const auto pursuit_begin = [&](const Entity &t) {
    if (pursue_key_x_ != t.abs_x || pursue_key_y_ != t.abs_y) {
      pursue_key_x_ = t.abs_x;
      pursue_key_y_ = t.abs_y;
      // 换目标：距离基准重算（对不同目标比距离没意义），tick 预算不重置——
      // 否则两个敌车交替当目标会让预算永远攒不起来（R5-P 首场实测追 46 tick）
      pursue_stall_ = 0;
      pursue_best_d_ = -1;
    }
  };
  const auto pursuit_tick = [&](const Entity &t) {
    ++pursue_ticks_;
    const int d = pursuit_dist(x, y, t);
    if (pursue_best_d_ < 0 || (d >= 0 && d < pursue_best_d_)) {
      pursue_best_d_ = d;
      pursue_stall_ = 0;
    } else {
      ++pursue_stall_;
    }
  };
  const auto pursuit_abandon = [&]() {
    pursue_key_x_ = -1;
    pursue_key_y_ = -1;
    pursue_ticks_ = 0;
    pursue_stall_ = 0;
    pursue_cool_ = params_.pursue_cooldown;
  };
  const auto pursuit_allowed = [&](const Entity &t) {
    const int manh = std::abs(t.abs_x - x) + std::abs(t.abs_y - y);
    const bool cornered = target_escape(t) <= params_.pursue_corner_escape;
    // 只有"真贴身"（相邻）与"压进死角"不受预算限制。Chebyshev ≤2 曾放进
    // 斜对角（dx=1,dy=1）——打不到又永远贴身，首场实测追出 46 tick 长段。
    if (cornered || manh <= 1) return true;
    if (pursue_cool_ > 0) return false;
    if (pursue_ticks_ >= params_.pursue_max_ticks) return false;
    if (pursue_stall_ >= params_.pursue_max_stall) return false;
    return true;
  };

  // 本 Tick 直线射程内的敌车（他们的攻击按我方移动前坐标结算，见 §5.2）
  // R1（10/28 自对弈）：防御中的敌车本 Tick 不反击，不记入 k_in
  std::vector<const Entity *> in_line;
  for (const auto &e : p.entities) {
    if (!IsEnemy(e, p)) continue;
    if (e.protected_flag) continue;  // 保护期内不能攻击
    if (e.defending) continue;       // 防御中不反击
    if (CanHit(e.abs_x, e.abs_y, x, y)) in_line.push_back(&e);
  }
  const int k_in = static_cast<int>(in_line.size());

  // 3) 攻击判定：两遍扫描（可击杀优先）+ 参数化门槛（B3 均衡版增强）；
  //    饱和门已在 R5-N 撤掉，攻击判定不按对抗分饱和分叉（V8.6c 复核）。
  //    R1 生存门（10/28 自对弈 round1：镜像同归于尽）：同 Tick 结算下，出手后
  //    仍被 k_in 路直线火力打死（k_in×attack_damage ≥ self_hp）的击杀一律
  //    放弃——同归于尽净分 0（击杀 +60、死亡 -60），还白送 3 Tick 复活窗口
  //    给其余对手渔利；绝不为收头赔命，交给下方防御/撤离兜底。
  if (!p.self.protected_flag &&
      (life_paid || k_in * params_.attack_damage < self_hp)) {
    // R5-E：本命回本后放宽 R1 生存门——同归于尽净 0（击杀 +60 / 死亡 -60），
    // 且本命已赚够下一条命的罚款，收头换命不亏。
    const Entity *pick = nullptr;
    int pick_d = INT_MAX;
    std::string pick_dir;
    const auto consider = [&](const Entity &e, int d, const std::string &dir) {
      // 同距 tie-break：方向字典序（down<left<right<up），保证确定
      if (d > pick_d || (d == pick_d && dir >= pick_dir)) return;
      pick = &e;
      pick_d = d;
      pick_dir = dir;
    };
    const auto candidate_ok = [&](const Entity &e, int dx, int dy) {
      // V7/R4-A（10/30 用户拍板"落井下石"）：防御中目标照打——只掉半伤
      // 且其本 Tick 不反击（k_in 已排除），白嫖伤害；仅保护期目标不可伤。
      if (!IsEnemy(e, p) || e.protected_flag) return false;
      // 方向内更近的敌车会先被命中（防御/保护也一样挡路）→ 白打，跳过
      for (const auto &o : p.entities) {
        if (&o == &e || !IsEnemy(o, p)) continue;
        const int odx = o.abs_x - x;
        const int ody = o.abs_y - y;
        const bool same_dir = (dx > 0 && odx > 0 && ody == 0) ||
                              (dx < 0 && odx < 0 && ody == 0) ||
                              (dy > 0 && ody > 0 && odx == 0) ||
                              (dy < 0 && ody < 0 && odx == 0);
        if (same_dir &&
            std::abs(odx) + std::abs(ody) < std::abs(dx) + std::abs(dy))
          return false;
      }
      return true;
    };
    const auto scan_dir = [](int dx, int dy) -> std::string {
      if (dx > 0) return "right";
      if (dx < 0) return "left";
      return dy > 0 ? "down" : "up";
    };
    // 第一遍：可击杀（hp ≤ attack_damage；V7/R4-A 防御目标按半伤折算，
    //   hp ≤ attack_damage_defended 才一枪收头）——击杀奖励 + 对手扣 D，
    //   永远划算
    for (const auto &e : p.entities) {
      const int dmg =
          e.defending ? params_.attack_damage_defended : params_.attack_damage;
      if (e.hp > dmg) continue;
      const int dx = e.abs_x - x;
      const int dy = e.abs_y - y;
      const bool straight =
          (dx == 0 && std::abs(dy) >= 1 && std::abs(dy) <= 2) ||
          (dy == 0 && std::abs(dx) >= 1 && std::abs(dx) <= 2);
      if (!straight || !candidate_ok(e, dx, dy)) continue;
      consider(e, std::abs(dx) + std::abs(dy), scan_dir(dx, dy));
    }
    // 第二遍："有利可图"目标（V6 起与饱和度无关——见 R5-N）；
    //   V6（10/29，用户定调"低血不能见人就跑、等天使等不到"）：取消 P3 的
    //   低血跳门——低血同样参与换血判定，但 safe_trade 要求严格领先一枪
    //   （e.hp < self_hp），即低血只打必赢的换血，不送头也不躺平；
    //   R2 终版（10/28 自对弈 round2 + 官方陪练三轮回归）：保留无条件
    //   IsIsolated 换血（官方陪练偶尔还手、换血总体白赚：旧代码伤害 70–110、
    //   排名 2/4；攻击倾向门 0.05 版只打出 40/0/0，一刀切与 ==0 门更差）。
    //   镜像对称换血拖到 20/20 的死锁改由 R1 生存门（20 血互杀被拦）+
    //   R2-B 回血路线（低血朝 LLM 校验的回血点走）兜底，不再靠攻击门压制。
    // R5-N（10/30 V8.2 实测后用户："饱和之后其实还是应该打，超过 500 分了其实
    // 还是有比较空间的……超过 500 应该也按 500 分以前的策略来"）：删掉 R5-M 的
    // "饱和时只还手打得赢的对射"特判——饱和与未饱和不再分叉。理由：① 我们
    // 520 时对手 C 已 654、只差 2.5 分，超 500 的击杀仍扣对手 60 D（相对排名
    // 有效）、还掐对手资源/任务；② 死亡代价（-60 D）本来就是未饱和换血公式
    // 的定价项（favorable/safe_trade + R1 生存门要求"本 Tick 不死且换血占优"），
    // 即"一条命能换的价值"早已算进门槛，不再需要饱和度另立一道闸。
    if (!pick) {
      const int low = 2 * params_.attack_damage;
      for (const auto &e : p.entities) {
        // 第一遍已看：按实际伤害折算的可击杀目标跳过（防御目标半伤）
        const int dmg = e.defending ? params_.attack_damage_defended
                                    : params_.attack_damage;
        if (e.hp <= dmg) continue;
        const int dx = e.abs_x - x;
        const int dy = e.abs_y - y;
        const bool straight =
            (dx == 0 && std::abs(dy) >= 1 && std::abs(dy) <= 2) ||
            (dy == 0 && std::abs(dx) >= 1 && std::abs(dx) <= 2);
        if (!straight || !candidate_ok(e, dx, dy)) continue;
        // R3-A（10/28 晚，用户拍板方向"该激进就大胆，主动寻求换血"）：②
        // 放宽为"同血即打"（原"严格领先一枪"）——官方陪练先手换血基本
        // 不还手/还手弱，均势换血白赚伤害；落后（self_hp < e.hp）仍不打
        // （换血赛跑必输），孤立目标条款不变；镜像 20/20 死锁仍由 R1
        // 生存门 + R2-B 回血路线兜底。
        const bool favorable =
            e.hp < low || self_hp >= e.hp || IsIsolated(e, p) || life_paid;
        const bool safe_trade =
            self_hp >= low || e.hp < self_hp || life_paid;
        // R5-E：本命回本后低血不再拦换血（favorable/safe_trade 均放行）——
        // 残血主动打伤害然后满状态复活，长期净赚。
        if (!favorable || !safe_trade) continue;
        consider(e, std::abs(dx) + std::abs(dy), scan_dir(dx, dy));
      }
    }
    if (pick) {
      return emit(ActionChoice::Attack(
          pick_dir, "白捡击杀：敌车 " + std::to_string(pick_d) + " 格（HP " +
                        std::to_string(pick->hp) + "）"));
    }
  }

  // 4) 防御保命（10/28 新增）：直线火力不防御必死、防御减半可活 → defend
  //    （移动躲不掉本 Tick 伤害——攻击按移动前坐标结算；防御是唯一减伤手段）
  if (!p.self.protected_flag && k_in >= 1) {
    const int undefended = k_in * params_.attack_damage;
    const int defended = k_in * params_.attack_damage_defended;
    if (undefended >= self_hp && defended < self_hp) {
      return emit(ActionChoice::Defend(
          "防御保命：直线火力 " + std::to_string(k_in) +
          " 路（不防御 " + std::to_string(undefended) + " 伤必死，防御 " +
          std::to_string(defended) + " 伤可活）"));
    }
  }

  // 4b) 必死还手（V8.3 实测，10/30 那场 tick 29）：本 Tick 连防御都救不了命时
  //    （k_in×attack_damage_defended ≥ self_hp），移动/防御都是纯亏——攻击按移动
  //    前坐标结算，移动躲不掉本 Tick 伤害；防御减半后仍然必倒，而反伤损血又
  //    "不计入防御方的普通攻击贡献"（rules v1.2 §10.2），拿不到伤害分。既然如此
  //    就把这一炮打出去：白赚 20 点实际伤害，目标残血还能顺手 +60 收割（击杀
  //    奖励另计，不受 500 截断影响）。挑选顺序：可一枪收头的目标 → 血量最低 →
  //    距离最近 → 方向字典序，取确定解。
  if (!p.self.protected_flag && k_in >= 1 &&
      k_in * params_.attack_damage_defended >= self_hp) {
    const auto dir_name = [](int dx, int dy) -> std::string {
      if (dx > 0) return "right";
      if (dx < 0) return "left";
      return dy > 0 ? "down" : "up";
    };
    const Entity *pick = nullptr;
    int pick_hp = INT_MAX, pick_d = INT_MAX;
    std::string pick_dir;
    for (const auto &e : p.entities) {
      if (!IsEnemy(e, p) || e.protected_flag) continue;
      const int dx = e.abs_x - x;
      const int dy = e.abs_y - y;
      const bool straight =
          (dx == 0 && std::abs(dy) >= 1 && std::abs(dy) <= 2) ||
          (dy == 0 && std::abs(dx) >= 1 && std::abs(dx) <= 2);
      if (!straight) continue;
      // 同方向更近的敌车先吃弹道（防御/保护也一样挡路）→ 打不到它
      bool shadowed = false;
      for (const auto &o : p.entities) {
        if (&o == &e || !IsEnemy(o, p)) continue;
        const int odx = o.abs_x - x;
        const int ody = o.abs_y - y;
        const bool same_dir = (dx > 0 && odx > 0 && ody == 0) ||
                              (dx < 0 && odx < 0 && ody == 0) ||
                              (dy > 0 && ody > 0 && odx == 0) ||
                              (dy < 0 && ody < 0 && odx == 0);
        if (same_dir &&
            std::abs(odx) + std::abs(ody) < std::abs(dx) + std::abs(dy)) {
          shadowed = true;
          break;
        }
      }
      if (shadowed) continue;
      const int d = std::abs(dx) + std::abs(dy);
      const std::string dir = dir_name(dx, dy);
      if (e.hp < pick_hp ||
          (e.hp == pick_hp && d < pick_d) ||
          (e.hp == pick_hp && d == pick_d && dir < pick_dir)) {
        pick = &e;
        pick_hp = e.hp;
        pick_d = d;
        pick_dir = dir;
      }
    }
    if (pick) {
      return emit(ActionChoice::Attack(
          pick_dir, "必死还手：本Tick " + std::to_string(k_in) +
                        " 路火力（防御也 " +
                        std::to_string(k_in * params_.attack_damage_defended) +
                        " 伤 ≥ HP " + std::to_string(self_hp) +
                        "），先打一炮 → 敌车 " + std::to_string(pick_d) +
                        " 格（HP " + std::to_string(pick->hp) + "）"));
    }
  }

  // 5a) 低血回血路线（R2-B，10/28 自对弈 round2 死锁修复）：撤离态没有自然
  //   回血、回滞解除线（hp ≥ 3×attack_damage）永远到不了 → 20 血车全场振荡
  //   挂机。出路 = LLM 校验结论中的增益点位（event_plan avoid=false，points
  //   或 rect）：低血（< 3×attack_damage）朝最近点走，入点即获得持续回血
  //   增益（离开不提前取消，无需驻留）。撤离态下优先于远敌评分；非撤离态下
  //   只在视野内无敌车时走（不与任务/采集抢优先级）。
  const bool heal_wanted = self_hp < 3 * params_.attack_damage;
  bool has_heal_target = false;
  int heal_tx = -1, heal_ty = -1;
  if (heal_wanted && has_conclusion_ && last_conclusion_.has_plan &&
      !last_conclusion_.plan.avoid &&
      last_conclusion_.plan.until_tick > p.tick) {
    int bd = INT_MAX;
    for (const auto &pt : last_conclusion_.plan.points) {
      const int d = std::abs(pt[0] - x) + std::abs(pt[1] - y);
      if (d < bd) {
        bd = d;
        heal_tx = pt[0];
        heal_ty = pt[1];
        has_heal_target = true;
      }
    }
    if (!has_heal_target && last_conclusion_.plan.has_rect) {
      heal_tx = (last_conclusion_.plan.rect_x0 + last_conclusion_.plan.rect_x1) /
                2;
      heal_ty = (last_conclusion_.plan.rect_y0 + last_conclusion_.plan.rect_y1) /
                2;
      has_heal_target = true;
    }
  }

  // 5) 低血避火（V6，10/29 用户定调"低血不能见人就跑、天使等不到"）：低血
  //    不再进入跨 Tick 撤离态；只在真实威胁下避让——视野（Chebyshev ≤4）内
  //    有敌车且（本 Tick 在其枪线下 threats≥1（含 3–4 格延伸线）或 被 ≥2 车
  //    近身）才做撤离评分
  //    （P1/P2 评分保留：最小枪线 → 最大最小敌距 → 开放邻居 → 远离梯度），
  //    威胁解除立即恢复任务/采集/巡逻。低血不逃命 = 继续得分；必输换血不送
  //    头（R1 生存门 + 二遍 safe_trade 领先一枪）；天使恩泽公告时优先回血。
  std::vector<const Entity *> visible;  // 视野内（Chebyshev ≤4）敌车
  for (const auto &e : p.entities) {
    if (!IsEnemy(e, p)) continue;
    if (std::max(std::abs(e.abs_x - x), std::abs(e.abs_y - y)) <= 4)
      visible.push_back(&e);
  }
  const bool visible_enemy = !visible.empty();
  // V8.6：最近敌车是否已在 2 格内（贴脸）。资源探索不在敌车鼻子底下盲走，
  // 贴脸时把 Tick 让给还手/防御/巡逻；已知资源仍由采集层正常去捡。
  const bool enemy_close = [&]() {
    for (const auto *e : visible)
      if (std::max(std::abs(e->abs_x - x), std::abs(e->abs_y - y)) <= 2)
        return true;
    return false;
  }();
  const bool low_hp = self_hp < 2 * params_.attack_damage;
  // 枪线威胁计数（撤离评分与任务/采集步进安全门共用；InGunLine 覆盖 3–4
  // 格的延伸枪线——低血时站在枪线上会被下一 Tick 拉进射程，同样要避）
  const auto threats_at = [&](int cx, int cy) {
    int t = 0;
    for (const auto *e : visible)
      if (InGunLine(e->abs_x, e->abs_y, cx, cy)) ++t;
    return t;
  };
  // 本 Tick 真实威胁（V8.6 资源优先，10/31 用户："哪怕为此掉血，只要不死
  // 也要多捡资源"）：低血 + 视野内有敌车 + 本 Tick 直线火力足以致命才避火。
  // 旧版"枪线下有威胁就撤"会把 30 血、单敌 20 伤的可承受掉血也当成必死，
  // 为了不掉 20 血放弃 10–30 分资源，不划算；只有 k_in×20 ≥ HP 才先保命。
  const int incoming_now = k_in * params_.attack_damage;
  const bool immediate_danger =
      low_hp && visible_enemy && incoming_now >= self_hp;
  // R5-E：本命回本后残血不再避战逃命——主动换伤害，死掉满状态复活（每条命
  // 赚 ≥80 即回本，死亡只扣 60）；防御保命层（4）保留：能活就活继续打。
  if (immediate_danger && !p.self.protected_flag && !life_paid) {
      const auto dist_to_nearest = [&](int cx, int cy) {
        int d = INT_MAX;
        for (const auto *e : visible)
          d = std::min(d, std::max(std::abs(e->abs_x - cx),
                                   std::abs(e->abs_y - cy)));
        return d;
      };
      const auto open_neighbors = [&](int cx, int cy) {
        int o = 0;
        if (!cell_blocked(cx, cy - 1)) ++o;
        if (!cell_blocked(cx, cy + 1)) ++o;
        if (!cell_blocked(cx - 1, cy)) ++o;
        if (!cell_blocked(cx + 1, cy)) ++o;
        return o;
      };
      const auto away_sum = [&](int cx, int cy) {
        int s = 0;
        for (const auto *e : visible)
          s += std::max(std::abs(e->abs_x - cx), std::abs(e->abs_y - cy));
        return s;
      };
      // R2-B：先试回血路线（有天使恩泽点位时，回血比逃离值钱）。安全门：
      // 首步枪线威胁不增（防穿过敌枪线送头）；不可达/更危险则退回常规撤离。
      if (has_heal_target) {
        const std::string hdir =
            BfsFirstStep(x, y, heal_tx, heal_ty, w, h, blocked);
        if (!hdir.empty()) {
          int hx = x, hy = y;
          if (hdir == "up") hy = y - 1;
          else if (hdir == "down") hy = y + 1;
          else if (hdir == "left") hx = x - 1;
          else hx = x + 1;
          if (threats_at(hx, hy) <= threats_at(x, y)) {
            return emit(ActionChoice::Move(
                hdir, "低血避火：朝回血点 [" + std::to_string(heal_tx) + "," +
                          std::to_string(heal_ty) + "] 移动（LLM 校验事件点位）"));
          }
        }
      }
      const int cur_t = threats_at(x, y);
      const int cur_d = dist_to_nearest(x, y);
      const int cur_open = open_neighbors(x, y);
      int bx = -1, by = -1;
      int best_t = INT_MAX, best_d = -1, best_open = -1, best_sum = -1;
      const int cand_x[4] = {x, x, x - 1, x + 1};
      const int cand_y[4] = {y - 1, y + 1, y, y};  // up/down/left/right
      for (int i = 0; i < 4; ++i) {
        const int nx = cand_x[i], ny = cand_y[i];
        if (cell_blocked(nx, ny)) continue;
        const int t = threats_at(nx, ny);
        const int d = dist_to_nearest(nx, ny);
        const int o = open_neighbors(nx, ny);
        const int s = away_sum(nx, ny);
        if (bx < 0 || t < best_t || (t == best_t && d > best_d) ||
            (t == best_t && d == best_d && o > best_open) ||
            (t == best_t && d == best_d && o == best_open && s > best_sum)) {
          bx = nx;
          by = ny;
          best_t = t;
          best_d = d;
          best_open = o;
          best_sum = s;
        }
      }
      if (bx >= 0) {
        // 撤离门（P1②）：威胁更小；或威胁不增且距离不缩（平移保距也算）；
        // 或全平但更开阔（P2① 角落/死路降权）
        if (best_t < cur_t || (best_t == cur_t && best_d >= cur_d) ||
            (best_t == cur_t && best_d == cur_d && best_open > cur_open)) {
          std::string dir;
          if (by == y - 1) dir = "up";
          else if (by == y + 1) dir = "down";
          else if (bx == x - 1) dir = "left";
          else dir = "right";
          return emit(ActionChoice::Move(
              dir, "低血避火（HP " + std::to_string(self_hp) + "，下格枪线 " +
                       std::to_string(best_t) + " 路，最小距 " +
                       std::to_string(best_d) + "）"));
        }
      }
      // 无合法/更优撤离格：低血 + 直线射程内 → 防御减伤兜底（比裸挨打强）
      if (k_in >= 1) {
        return emit(ActionChoice::Defend(
            "低血被围且无合法撤离格，防御减伤（直线火力 " +
            std::to_string(k_in) + " 路）"));
      }
  }

  // 5b) 低血回血路线（R2-B；V8.6 放宽）：低血且本 Tick 不致命（不要求视野内
  //   无敌车）→ 朝回血点走，优先于任务/采集与游走。必死场景仍由 5) 避火接管。
  if (has_heal_target && !immediate_danger && !p.self.protected_flag) {
    const std::string hdir =
        BfsFirstStep(x, y, heal_tx, heal_ty, w, h, blocked);
    if (!hdir.empty()) {
      return emit(ActionChoice::Move(
          hdir, "低血回血：朝回血点 [" + std::to_string(heal_tx) + "," +
                    std::to_string(heal_ty) + "] 移动（LLM 校验事件点位）"));
    }
  }

  // 6/7) 采集与任务（10/28：我方资源分已饱和 → 任务优先，selftest 19）
  const bool mission_first = belief_.our_scores.resource_saturated;
  // V8.6（10/31 用户："最重要的还是资源，哪怕为此掉血，只要不死也要多捡"）：
  //   资源未拿满且未锁存"采尽"前，采集层拥有探索权——没有已知资源就主动找，
  //   不再回 wait 让巡逻/追猎把时间烧掉（15 场压测：迷宫图巡逻 364 Tick、
  //   采集只有 36 Tick，资源 30/200）。
  // 资源任务（normal_resources / high_resources / resource_value）没完成时，
  // 即使资源分已饱和/已锁存，也继续把资源当目标——任务分 5 分/个，且高价值
  // 资源往往就是没做完的那个任务（迷宫局 32.5 的另一半缺口）。
  const bool resource_mission_pending = [&]() {
    for (const auto &m : p.missions) {
      if (m.type.find("resource") == std::string::npos) continue;
      bool done = false;
      for (const auto &id : p.score.mission_completed_ids)
        if (id == m.id) done = true;
      if (!done) return true;
    }
    return false;
  }();
  const bool resource_pending =
      (!belief_.our_scores.resource_saturated && !economy_latched_) ||
      resource_mission_pending;
  // 探索找资源只在"标准对局"里开：还有未完成任务，或已完成过任务（赛后余量
  // 收割）。没有任务列表的自定义局退回巡逻，避免无目的全图乱走。
  const bool mission_pending = [&]() {
    for (const auto &m : p.missions) {
      bool done = false;
      for (const auto &id : p.score.mission_completed_ids)
        if (id == m.id) done = true;
      if (!done) return true;
    }
    return false;
  }();
  const bool explore_for_resources =
      resource_pending && !enemy_close &&
      (mission_pending || p.score.mission_completed > 0);
  // V6（10/29）步进安全门，V8.6 放宽：低血且视野内有敌车时，任务/采集的下一步
  //   只要"本 Tick 不会被打死"就放行——掉 20 血换 10–30 分资源是赚的；会致死
  //   才让位给避火/防御（真正的保命在 5) 低血避火与 4) 防御保命）。
  const auto step_is_lethal = [&](const std::string &d) {
    if (!low_hp || !visible_enemy) return false;
    int nx = x, ny = y;
    if (d == "up") ny = y - 1;
    else if (d == "down") ny = y + 1;
    else if (d == "left") nx = x - 1;
    else nx = x + 1;
    return threats_at(nx, ny) * params_.attack_damage >= self_hp;
  };
  const auto try_mission = [&]() -> bool {
    for (const auto &m : p.missions) {
      if (!m.has_position) continue;
      // V4 任务销账（10/29 复盘实测）：裁判全场保留 missions 列表（499 tick
      // 场次中 reach 任务完成后仍逐帧列出），未过滤会把车反复拉回已完成任务
      // 点——实测任务完成后仍 217 次"朝任务点 [9,9]"、600 帧站中心被集火。
      // 已完成 id（感知层 score.mission_completed_ids）直接跳过，不再回头。
      bool done = false;
      for (const auto &id : p.score.mission_completed_ids)
        if (id == m.id) done = true;
      if (done) continue;
      if (m.pos_x == x && m.pos_y == y) continue;  // 已站在点上，结算时完成
      std::string dir = BfsFirstStep(x, y, m.pos_x, m.pos_y, w, h, blocked);
      if (dir.empty()) continue;  // 不可达 → 换下一个任务
      // 上一步同方向被挡 → 轮换一次，避免对着墙硬撞
      if (p.self.has_last_action && p.self.last_action == "move" &&
          !p.self.last_moved && p.self.has_last_direction &&
          p.self.last_effective_direction == dir) {
        dir = RotateDir(dir);
      }
      if (step_is_lethal(dir)) continue;  // V8.6 安全门：低血只避必死步
      emit(ActionChoice::Move(
          dir, "朝任务点 [" + std::to_string(m.pos_x) + "," +
                   std::to_string(m.pos_y) + "] 推进"));
      return true;
    }
    return false;
  };
  // V8.6：任务/采集的下一步只在"本 Tick 会死"时跳过（见上方 step_is_lethal）。
  if (mission_first && try_mission()) return cached_;
  // V8.6b（10/31 下午，迷宫 dog 回归）：资源分已饱和、伤害分还有空间、且没有
  // 未完成的资源任务时，再捡资源不涨分（30×min(R/120,1) 已封顶），反而把
  // Tick 从伤害上拿走——迷宫 dog 场因此从 1580 伤害/60 分掉到 240/52.2。
  // 这时把 Tick 还给任务/巡逻/战斗；伤害也饱和后仍可顺手捡（掐对手资源）。
  const bool skip_collect_for_combat =
      belief_.our_scores.resource_saturated &&
      !belief_.our_scores.combat_saturated && !resource_mission_pending;
  if (!skip_collect_for_combat) {
    // V8.6：资源未拿满 → 采集层允许探索找资源；阶段二的 E_BLOCKED 不学成墙
    // （敌车占位/幻术反转），并把幻术状态带进去。
    collector_.set_dynamic_blockers(true);
    collector_.set_illusion(illusion_);
    collector_.set_explore_enabled(explore_for_resources);
    ActionChoice c = collector_.decide(p, llm);
    if (c.action == "move" && !step_is_lethal(c.direction)) {
      return emit(ActionChoice::Move(
          c.direction, "采集：" + c.reason +
                           (resource_pending ? "（资源优先）" : "")));
    }
  }
  if (!mission_first && try_mission()) return cached_;

  // 8) 游走兜底：环形巡逻（见下）；此前先做多敌避让与满血单敌追猎。
  //    V6（10/29）：删除"低血游走：远离最后已知敌车"——低血整场空转逃命
  //    不得分（实测 109+95 tick 用于跑路、伤害只有 20）；真实威胁由低血避
  //    火块处理，其余时间低血照常巡逻/采集/做任务。
  std::string dir;
  // R5-M（10/30 V8.2 实测，用户："在交火范围了又往前走一步让别人白打一下"）：
  // 非参战移动（巡逻/兜底）不走入、不停留在敌车直线射程（1–2 格）内——
  // 被白打不还手是纯亏（本 Tick 伤害按移动前坐标结算，踏入后下一 Tick 才挨打）。
  // 已在射程内 → 优先一步走出射程；当前安全 → 巡逻下一步会踏进射程时换方向。
  // 四邻都不安全就保持原方向（交给低血避火/贴脸还手兜底），不硬凑。
  const auto avoid_enemy_fire = [&](const std::string &want) {
    if (want.empty() || visible.empty()) return want;
    const auto in_any_range = [&](int cx, int cy) {
      for (const auto *e : visible)
        if (CanHit(e->abs_x, e->abs_y, cx, cy)) return true;
      return false;
    };
    const int ax4[4] = {x, x + 1, x, x - 1};
    const int ay4[4] = {y - 1, y, y + 1, y};
    const char *ad4[4] = {"up", "right", "down", "left"};
    int nx = x, ny = y;
    if (want == "up") ny -= 1;
    else if (want == "down") ny += 1;
    else if (want == "left") nx -= 1;
    else nx += 1;
    if (!in_any_range(x, y) && !in_any_range(nx, ny)) return want;
    std::string alt;
    for (int i = 0; i < 4; ++i) {
      if (cell_blocked(ax4[i], ay4[i])) continue;
      if (in_any_range(ax4[i], ay4[i])) continue;
      if (ad4[i] == want) return want;  // 原方向本身就安全
      if (alt.empty()) alt = ad4[i];
    }
    return alt.empty() ? want : alt;
  };
  // V7/R4-A（10/30，用户拍板"任务/资源分作为参战信念"）：主动找架（多敌
  // 集火参与 / 单敌追猎）统一挂参战开关——存在未完成的 effective_damage
  // 战斗任务，或（无任何未完成任务 且 资源分已饱和）才主动参战；否则任务/
  // 资源没拿满 → 多敌维持 R2-C 避让、单敌不追（贴脸被动攻击/防御不受门限）。
  const bool damage_mission_active = [&]() {
    for (const auto &m : p.missions) {
      if (m.type != "effective_damage") continue;
      bool done = false;
      for (const auto &id : p.score.mission_completed_ids)
        if (id == m.id) done = true;
      if (!done) return true;
    }
    return false;
  }();
  const bool no_pending_missions = [&]() {
    for (const auto &m : p.missions) {
      bool done = false;
      for (const auto &id : p.score.mission_completed_ids)
        if (id == m.id) done = true;
      if (!done) return false;
    }
    return true;
  }();
  // R5-A（10/30 V8 练习赛实测）："资源采尽"信号补入参战门——V7 首场实测全图
  // 资源总分 200、我方 60，饱和线 120 永远不可达 → 参战门全程关闭（53 次多敌
  // 避让、0 集火）。环形巡逻 + 4 格视野可扫完全图：信念记忆里所有已知资源都
  // 已在视野内消失、且当前无可见资源 = 无资源可采（≈ 饱和）。顺路发现新资源
  // 时采集层优先级更高，会自动回采。
  const bool resources_done = [&]() {
    for (const auto &e : p.entities)
      if (e.type == "resource_normal" || e.type == "resource_high") return false;
    return belief_.map.resources_known.empty();
  }();
  // R5-F/R5-G（10/30 V8，用户拍板"不能确定真实地图、角落可能藏资源"）：采尽
  // 判定须有一轮巡逻实证，成立即锁存——此后偶见资源由采集层顺手捡，不把参战门
  // 关回去。R5-G（V8 练习赛实测）：顺序指针"绕回环首"被多敌避让/死亡反复劫持，
  // 实测 tick 470+ 才凑满一圈（全场集火参与仅 1 次、单敌追猎 2 次），而踩过的
  // 环格 28/28 其实 tick 317 就覆盖完了——实证改为踩格覆盖率（≥ 默认 90%）。
  {
    const std::vector<Cell> ring_now = BuildPatrolRing(w, h);
    const double ring_cov =
        ring_now.empty()
            ? 0.0
            : static_cast<double>(ring_visited_.size()) /
                  static_cast<double>(ring_now.size());
    // V8.6（10/31 15 场压测）：这里不再加"全图覆盖率"硬门槛——资源未拿满时
    // 采集层一直在探索（见 6/7 节），几乎不给环形巡逻累积覆盖的机会；等探索
    // 把可达前沿走完、采集层回 wait 之后，环覆盖才会攒满，锁存自然发生在
    // "确实没资源可找"之后。真·采尽判定仍由"可见无资源 + 信念记忆空"兜底。
    if (resources_done && ring_cov >= params_.patrol_lap_coverage)
      economy_latched_ = true;
  }
  // R5-J（10/30 V8.2，用户："血量比对面高就应该追着打了，没必要做那么高的
  // 限制"）：参战开关关（经济未锁存/任务未清）时，血量高于视野内最强敌车即
  // 主动参战——多敌走 R4-A 集火最低血、单敌走 R3-A 追猎。撤掉 R5-H 的
  // "≥3×attack_damage 且 ≥2×最低血"高门槛；任务/采集层优先级更高不抢，
  // R4-A 自带 R1 生存门/威胁步进安全门保留（低血追进枪线仍会被拦）；饱和门
  // 已在 R5-N 撤掉，V8.6c 再次确认不按对抗分饱和停追。
  const bool hp_dominant = [&]() {
    int mx = 0;
    for (const auto *e : visible) mx = std::max(mx, e->hp);
    return !visible.empty() && self_hp > mx;
  }();
  // V8.6（10/31 用户："最重要的还是资源，哪怕为此掉血，只要不死也要多捡"）：
  // 资源没拿满、且最近敌车不在 2 格内（不是贴脸自保）时，不主动开追猎/集火。
  // 注意：这条门只看"资源是否拿满"，**绝不看对抗分饱和**——10/31 用户再次
  // 确认 R5-N：超 500 的 D 虽不计入自己那 15 分，但同分比较用未截断的有效
  // 伤害，且每次击杀扣对手 60 D，相对排名仍值钱。V8.6 曾误加 combat_capped
  // 把饱和后的 hp_dominant 追猎关掉，已撤销并由 selftest 92 锁死。
  // 伤害任务在身 / 贴脸还手 / 本命回本敢死队不受此门限制。
  const bool resource_priority = resource_pending && !enemy_close;
  const bool engage_wanted =
      damage_mission_active ||
      (hp_dominant && !resource_priority) ||
      (no_pending_missions &&
       (belief_.our_scores.resource_saturated || economy_latched_));
  {
    // R5-L（10/30 V8.2 实测后用户拍板："我们还有多敌避让吗？不是尽可能参战
    // 集火吗；血量低并且赚够分了那就送一波"）：删除 R2-C"多敌聚集避让"整段。
    // V8.2 练习赛 tick 69–84 实测：三车满血在 3 格外本机就在 (5,12)↔(6,12)
    // 来回横跳 16 tick（逃逸方向每 tick 无记忆重算，两侧都有敌人时平票翻转，
    // 与巡逻前进方向互相拉扯）。现在多敌不再逃跑：血量压过最低血目标就集火
    // （R4-A），本命回本（life_paid）直接参战"送一波"；占不到便宜就照常巡逻/
    // 采集（贴脸时由攻击判定还手）。真正的低血避战只留"血量低且本命未回本"
    // （5) 低血避火，R5-E 回本后连它也不拦）。
    const Entity *weakest = nullptr;
    for (const auto *e : visible) {
      if (e->protected_flag) continue;  // 保护期不可伤，不作为集火对象
      if (weakest == nullptr || e->hp < weakest->hp ||
          (e->hp == weakest->hp &&
           std::max(std::abs(e->abs_x - x), std::abs(e->abs_y - y)) <
               std::max(std::abs(weakest->abs_x - x),
                        std::abs(weakest->abs_y - y)))) {
        weakest = e;
      }
    }
    const bool hp_edge_weakest = weakest != nullptr && self_hp > weakest->hp;
    // R4-A（10/30 V7，用户拍板"人多聚堆就上前劝架、防御目标落井下石"）：
    // 参战开关开时，挑视野内血量最低的敌车（集火对象）逼近；防御中目标照打
    // （半伤落井下石），仅保护期目标不可伤跳过。
    // 安全门：① 低血（< 2×attack_damage）不参与（低血避火块接管）；
    // ② R1 生存门：本 Tick 枪线必死（k_in×attack_damage ≥ self_hp）不追；
    // ③ 原"对抗分饱和且无战斗任务 → 只追一枪可击杀"已由 R5-N 撤掉（饱和后
    //    照常追：击杀扣对手 D、掐资源，相对排名仍值钱）；
    // ④ HP < 3×attack_damage 时不得走新增枪线威胁的格。
    if (visible.size() >= 2 &&
        (engage_wanted ||
         (hp_edge_weakest && !resource_priority) ||
         life_paid)) {
      const Entity *target = nullptr;
      for (const auto *e : visible) {
        if (e->protected_flag) continue;  // 保护期不可伤
        if (target == nullptr || e->hp < target->hp ||
            (e->hp == target->hp &&
             std::max(std::abs(e->abs_x - x), std::abs(e->abs_y - y)) <
                 std::max(std::abs(target->abs_x - x),
                          std::abs(target->abs_y - y)))) {
          target = e;  // 最低血优先，同血取更近
        }
      }
      const bool pursue_ok =
          target != nullptr &&
          (life_paid || self_hp > target->hp) &&  // R5-J：血高于目标即追（原 ≥2×damage）
          (life_paid || k_in * params_.attack_damage < self_hp);
      // R5-E：回本后绕过①低血与②生存门槛（R5-N 起饱和不再另设门槛）
      if (pursue_ok) {
        pursuit_begin(*target);
        if (!pursuit_allowed(*target)) {
          pursuit_abandon();  // R5-P：追不动了 → 交回巡逻/采集，不再陪跑
        } else {
        const int cur_d = pursuit_dist(x, y, *target);
        const int cur_esc = target_escape(*target);
        // R5-Q 封角（10/31 用户："能把对面压进角落就追到死"）：目标已贴边或
        // 只剩 ≤2 个出口时，优先挑"能再封住一个口"的逼近格，把它往墙角赶。
        const bool herd = cur_esc <= 2 || target->abs_x <= 2 ||
                          target->abs_x >= w - 3 || target->abs_y <= 2 ||
                          target->abs_y >= h - 3;
        const int cur_t = threats_at(x, y);
        const int ax[4] = {x, x, x - 1, x + 1};
        const int ay[4] = {y - 1, y + 1, y, y};  // up/down/left/right
        const char *ad[4] = {"up", "down", "left", "right"};
        int best_d = INT_MAX;
        int best_t = INT_MAX;
        int best_esc = INT_MAX;
        std::string best_dir;
        for (int i = 0; i < 4; ++i) {
          if (cell_blocked(ax[i], ay[i])) continue;
          const int d = pursuit_dist(ax[i], ay[i], *target);
          if (d < 0) continue;
          // 切实逼近；或一步后目标进入射程（斜邻目标先摆正枪线再打）
          if (d >= cur_d &&
              !CanHit(ax[i], ay[i], target->abs_x, target->abs_y))
            continue;
          const int t = threats_at(ax[i], ay[i]);
          if (t > cur_t && self_hp < 3 * params_.attack_damage && !life_paid)
            continue;  // ④ 安全门（R5-E：回本后放行）
          const int esc = herd ? escape_after(ax[i], ay[i], *target) : INT_MAX;
          if (esc < best_esc ||
              (esc == best_esc &&
               (d < best_d || (d == best_d && t < best_t)))) {
            best_dir = ad[i];
            best_d = d;
            best_t = t;
            best_esc = esc;
          }
        }
        if (!best_dir.empty()) {
          pursuit_tick(*target);
          last_was_pursuit_ = true;
          wander_dir_ = best_dir;
          return emit(ActionChoice::Move(
              best_dir, "集火参与（" + std::to_string(visible.size()) +
                            " 车）：逼近最低血目标 HP " +
                            std::to_string(target->hp) +
                            (target->defending ? "（防御中，落井下石）" : "")));
        }
        }
      }
      // 无目标/安全门不放行/无逼近格 → 回退环形巡逻
    }
    // R3-A（10/28 晚，用户拍板方向"该激进就大胆，主动寻求换血"）：视野内
    // 恰 1 辆敌车、参战开关开、我方血量不低于对方（R5-J：血高即追，原门槛
    // ≥3×attack_damage）、R5-N 起对抗分饱和同样追猎，游走兜底改为主动逼近
    // 单敌——靠近后由攻击判定在 1–2 格射程内换血（同血即打，R3-A ①）；≥2 敌
    // 走上方 R4-A 集火参与（R5-L 已删 R2-C 避让），目标保护期不可伤不追；
    // V7 起单敌追猎同样挂参战开关（任务/资源没拿满不主动找架）。
    if (visible.size() == 1 && engage_wanted) {
      const Entity *solo = visible[0];
      if ((life_paid || self_hp >= solo->hp) && !solo->protected_flag) {
        pursuit_begin(*solo);
        if (!pursuit_allowed(*solo)) {
          pursuit_abandon();  // R5-P：风筝免疫——追不动就停，回去巡逻/采集
        } else {
        const int cur_d = pursuit_dist(x, y, *solo);
        const int cur_esc = target_escape(*solo);
        const bool herd = cur_esc <= 2 || solo->abs_x <= 2 ||
                          solo->abs_x >= w - 3 || solo->abs_y <= 2 ||
                          solo->abs_y >= h - 3;
        const int ax[4] = {x, x, x - 1, x + 1};
        const int ay[4] = {y - 1, y + 1, y, y};  // up/down/left/right
        const char *ad[4] = {"up", "down", "left", "right"};
        int best_d = INT_MAX;
        int best_esc = INT_MAX;
        for (int i = 0; i < 4; ++i) {
          if (cell_blocked(ax[i], ay[i])) continue;
          const int d = pursuit_dist(ax[i], ay[i], *solo);
          if (d < 0) continue;
          if (d >= cur_d && !CanHit(ax[i], ay[i], solo->abs_x, solo->abs_y))
            continue;
          const int esc = herd ? escape_after(ax[i], ay[i], *solo) : INT_MAX;
          if (esc < best_esc ||
              (esc == best_esc && d < best_d)) {
            dir = ad[i];
            best_d = d;
            best_esc = esc;
          }
        }
        if (!dir.empty()) {
          pursuit_tick(*solo);
          last_was_pursuit_ = true;
          wander_dir_ = dir;
          return emit(ActionChoice::Move(dir, "单敌追猎：满血主动寻求换血（R3-A）"));
        }
        }
      }
    }
  }
  // 8) 游走兜底：环形巡逻（10/29 V5，用户拍板"金边银肚皮草角"——角=出生/
  //    复活点危险区，边比中腹更安全）。距边缘 D 格的圆角方形环顺时针巡逻：
  //    不在环上 → 找最近可达路点归环；在环上 → 沿环推进下一路点；路点被
  //    障碍/敌车占 → 顺环向后找下一个可达路点。环不可用（极小图/全被堵）
  //    → 回退旧中心游走。
  {
    const std::vector<Cell> ring = BuildPatrolRing(w, h);
    if (!ring.empty()) {
      const Cell me{x, y};
      int me_idx = -1;
      for (int i = 0; i < static_cast<int>(ring.size()); ++i)
        if (ring[static_cast<size_t>(i)] == me) {
          me_idx = i;
          break;
        }
      if (patrol_w_ != w || patrol_h_ != h ||
          patrol_idx_ < 0 || patrol_idx_ >= static_cast<int>(ring.size())) {
        patrol_w_ = w;
        patrol_h_ = h;
        // R5-D（10/30 V8）：复位/换图时若已在环上则锚定当前环格（V5 语义），
        // 否则从环首开始
        patrol_idx_ = me_idx >= 0 ? me_idx : 0;
        ring_visited_.clear();  // R5-G：换图后巡逻实证作废，重新攒覆盖
      }
      // R5-G（10/30 V8 实测）：踩过的环格记入覆盖集——实证按覆盖率计，
      // 不要求顺序指针绕回环首（多敌避让/死亡劫持会把它拖到 tick 470+）。
      if (me_idx >= 0) ring_visited_.insert(me);
      // R5-D（10/30 V8 实测）：仅在"恰好到达当前目标路点"时沿环推进——V6 的
      // "在环上就推进"会在脱环绕行途中劫持锚定（WorldSplit 鬼墙绕行每经过一个
      // 环格就被改道，12 帧闭环在 (9,9)–(12,12) 空转 116 tick 到不了对岸路点）。
      if (me_idx >= 0 && me == ring[static_cast<size_t>(patrol_idx_)]) {
        const int next = (me_idx + 1) % static_cast<int>(ring.size());
        patrol_idx_ = next;
        ring_detour_ = false;  // R6-C：到达路点，解除绕行锁存
      } else if (me_idx < 0 && (just_respawned || teleported)) {
        // V6（10/29 修复）：仅真正重生时就近归环——脱环（采集/避火/追猎）后
        // 保持原环序推进，不再吸附曼哈顿最近路点。V5 的"脱环就近归环"会把
        // 车永久拽回出生区一角的环段（实测 500ms 场：左上环段 58/50/48 次、
        // 右侧与下边各 1–3 次，整场在偏离中心的左上角徘徊不成圈）。
        // R5-I（10/30 V8.1）：裁判可能跳过死亡帧直接复活满血 → 位置瞬移
        // >1 格视同重生（teleported），同样就近归环，不再 BFS 直插对岸。
        int best_md = INT_MAX;
        for (int i = 0; i < static_cast<int>(ring.size()); ++i) {
          const Cell wp = ring[static_cast<size_t>(i)];
          if (cell_blocked(wp.first, wp.second)) continue;
          const int md = std::abs(wp.first - x) + std::abs(wp.second - y);
          if (md >= best_md) continue;
          if (!BfsFirstStep(x, y, wp.first, wp.second, w, h, blocked).empty()) {
            best_md = md;
            patrol_idx_ = i;
          }
        }
        ring_detour_ = false;  // R6-C：重生归环后重新按环序推进
      }
      // 从 patrol_idx_ 顺环（顺时针）找第一个可达路点
      int best_idx = -1;
      for (int k = 0; k < static_cast<int>(ring.size()); ++k) {
        const int i = (patrol_idx_ + k) % static_cast<int>(ring.size());
        const Cell wp = ring[static_cast<size_t>(i)];
        if (wp == me) continue;
        if (cell_blocked(wp.first, wp.second)) continue;
        if (!BfsFirstStep(x, y, wp.first, wp.second, w, h, blocked).empty()) {
          best_idx = i;
          break;
        }
      }
      // 顺环全被堵 → 全环找曼哈顿最近可达路点保底
      if (best_idx < 0) {
        int best_md = INT_MAX;
        for (int i = 0; i < static_cast<int>(ring.size()); ++i) {
          const Cell wp = ring[static_cast<size_t>(i)];
          if (wp == me) continue;
          if (cell_blocked(wp.first, wp.second)) continue;
          const int md = std::abs(wp.first - x) + std::abs(wp.second - y);
          if (md >= best_md) continue;
          if (!BfsFirstStep(x, y, wp.first, wp.second, w, h, blocked).empty()) {
            best_md = md;
            best_idx = i;
          }
        }
      }
      if (best_idx >= 0) {
        patrol_idx_ = best_idx;
        const Cell wp = ring[static_cast<size_t>(best_idx)];
        // R5-I（10/30 V8.1，用户："中间有一段时间从地图中间横穿过去而不是
        // 沿着环走"）：在环上时沿环朝目标路点推进（顺/逆时针取短弧，等距
        // 顺时针），不再 BFS 直线切腹地（V8 实测路点被敌占跳过时，BFS 从
        // (9,5) 直插 (12,10) 走地图中间）。邻环格被占 → 落回 BFS 兜底。
        if (me_idx >= 0 && !ring_detour_) {
          const int n = static_cast<int>(ring.size());
          const int cw = (best_idx - me_idx + n) % n;
          const int ccw = (me_idx - best_idx + n) % n;
          const int step_idx =
              (cw <= ccw) ? (me_idx + 1) % n : (me_idx - 1 + n) % n;
          const Cell nxt = ring[static_cast<size_t>(step_idx)];
          if (!cell_blocked(nxt.first, nxt.second)) {
            if (nxt.first == x + 1 && nxt.second == y) dir = "right";
            else if (nxt.first == x - 1 && nxt.second == y) dir = "left";
            else if (nxt.first == x && nxt.second == y + 1) dir = "down";
            else if (nxt.first == x && nxt.second == y - 1) dir = "up";
          } else {
            // R6-C（10/31 巡逻仿真）：邻环格被占 → 进入绕行锁存，交给 BFS
            // 绕过去；锁存期间不再走环序步进（否则下一 Tick 的环序会把车
            // 推回原格，A↔B 两格死循环），直到抵达目标路点才恢复。
            ring_detour_ = true;
          }
        }
        if (dir.empty()) {
          dir = BfsFirstStep(x, y, wp.first, wp.second, w, h, blocked);
        }
        if (!dir.empty()) {
          if (p.self.has_last_action && p.self.last_action == "move" &&
              !p.self.last_moved && p.self.has_last_direction &&
              p.self.last_effective_direction == dir) {
            dir = RotateDir(dir);  // 上一步同方向被挡 → 轮换
          }
          // V8.6 安全门（原 V6）：低血且视野内有敌车时，环上步进只避"必死格"；
          // 可承受的枪线不绕，免得巡逻被 20 血换来换去拖成原地振荡。
          if (step_is_lethal(dir)) {
            const int ax2[4] = {x, x + 1, x, x - 1};
            const int ay2[4] = {y - 1, y, y + 1, y};  // up/right/down/left
            const char *ad2[4] = {"up", "right", "down", "left"};
            int best_t = INT_MAX;
            std::string alt;
            for (int i = 0; i < 4; ++i) {
              if (cell_blocked(ax2[i], ay2[i])) continue;
              const int t = threats_at(ax2[i], ay2[i]);
              if (t < best_t) {
                best_t = t;
                alt = ad2[i];
              }
            }
            if (!alt.empty() && !step_is_lethal(alt)) dir = alt;
          }
          dir = avoid_enemy_fire(dir);
          wander_dir_ = dir;
          return emit(ActionChoice::Move(
              dir, "环形巡逻：顺时针绕环 → [" + std::to_string(wp.first) +
                       "," + std::to_string(wp.second) + "]"));
        }
      }
    }
    // 回退：旧中心游走（环不存在 / 路点全被堵）
    const int cx = w / 2;
    const int cy = h / 2;
    dir = BfsFirstStep(x, y, cx, cy, w, h, blocked);
    if (dir.empty() && x == cx && y == cy) {
      dir = wander_dir_.empty() ? "up" : wander_dir_;
    }
    if (dir.empty()) {
      // 中心不可达：按固定序找第一个可走方向（地图被围兜底）
      const int cand_x[4] = {x, x + 1, x, x - 1};
      const int cand_y[4] = {y - 1, y, y + 1, y};  // up/right/down/left
      const char *cand_d[4] = {"up", "right", "down", "left"};
      for (int i = 0; i < 4; ++i) {
        if (!cell_blocked(cand_x[i], cand_y[i])) {
          dir = cand_d[i];
          break;
        }
      }
      if (dir.empty()) return emit(ActionChoice::Wait("游走无路可走"));
    }
  }
  if (p.self.has_last_action && p.self.last_action == "move" &&
      !p.self.last_moved && p.self.has_last_direction &&
      p.self.last_effective_direction == dir) {
    dir = RotateDir(dir);  // 上一步同方向被挡 → 轮换
  }
  dir = avoid_enemy_fire(dir);
  wander_dir_ = dir;
  return emit(ActionChoice::Move(dir, "游走探索（朝地图中心）"));
}

void CombatStrategy::on_new_session(const std::string &session_id) {
  collector_.on_new_session(session_id);
  belief_ = Belief{};  // 清空信念（障碍记忆/出生点/对手估计全部归零）
  wander_dir_.clear();
  patrol_idx_ = 0;  // 环形巡逻复位（V5；换场/换图后 patrol_w_/h_ 与地图比对自动重建）
  patrol_w_ = 0;
  patrol_h_ = 0;
  ring_detour_ = false;  // R6-C 绕行锁存复位
  alive_prev_ = true;  // V6 重生检测复位
  illusion_ = false;          // R5-B 幻觉状态复位（换场重新用回执证据学习）
  last_emitted_dir_.clear();
  life_baseline_set_ = false;  // R5-E 本命基线复位
  life_baseline_ = 0.0;
  ring_visited_.clear();      // R5-G 巡逻实证/参战锁存复位
  economy_latched_ = false;
  pursue_key_x_ = -1;         // R5-P 追击纪律复位
  pursue_key_y_ = -1;
  pursue_ticks_ = 0;
  pursue_best_d_ = -1;
  pursue_stall_ = 0;
  pursue_cool_ = 0;
  last_was_pursuit_ = false;
  last_x_ = -1;               // R5-I 瞬移检测复位
  last_y_ = -1;
  has_conclusion_ = false;  // LLM 结论复位（R2-B：跨场不沿用旧事件点位）
  last_conclusion_ = LlmConclusion{};
  has_cached_ = false;
  last_tick_ = -2;
  cached_ = ActionChoice{};
}

}  // namespace rmtc_bot
