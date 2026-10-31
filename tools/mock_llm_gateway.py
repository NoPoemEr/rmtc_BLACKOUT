#!/usr/bin/env python3
# mock_llm_gateway.py —— 自对弈专用：模拟官方 AI 服务的事件理解能力
#
# 为什么需要：练习裁判自带的 LLM 网关是 practice-mock，永远返回
#   {"event_plan": null, "summary": "模拟响应，仅验证通信，未理解事件"}
#   自对弈里选手端"LLM 结论 → 本地校验 → 事件应对"的闭环永远跑不到
#   事件分支。本工具按公告原文生成结构化事件理解，等效于"一个能正确
#   理解事件的大模型"，把正式赛环境的那一段管道在自对弈中补上。
#
# 合规边界：本文件是测试工具（模拟官方服务端），不属于参赛程序；
#   参赛程序侧绝不解析 event_text 原文（rules §8.1），只消费本服务
#   返回的 JSON 结论并经 parse_conclusion/validate_conclusion 校验。
#   本工具对提示词的解析只发生在测试服务端，不进入任何参赛代码。
#
# 行为（按最后一条 user 消息的公告原文生成结论）：
#   - 天使恩泽 → event_plan{points, avoid:false, until_tick=公告起始+存在时长-1}
#     已消失且超过有效期 → null；
#   - 爆炸 → event_plan{rect, avoid:true}；幻术 → {rect, avoid:true}；
#     世界分裂 → {points, avoid:true}（交点，线上危险）；
#   - 无相关公告 → event_plan null。
#   goals 取提示词中出现的任务类型（去重保序）。
#
# 用法: python3 tools/mock_llm_gateway.py [端口]     # 默认 8899
# 自对弈接入: SELFPLAY_LLM_GATEWAY=http://127.0.0.1:8899/v1/chat/completions \
#              ./tools/selfplay.sh 1 100 8080
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8899


def extract_announces(prompt):
    """从公告原文中提取天使/爆炸/幻术/世界分裂的最近一次有效参数。"""
    out = {"angel": None, "explosion": None, "illusion": None, "split": None}
    # 当前 tick（llm_brain 用户消息里的 tick=N）
    m = re.search(r"tick=(\d+)", prompt)
    current = int(m.group(1)) if m else 0
    # 天使恩泽：每个预告行独立解析，取"仍在有效期内"的最晚一个
    for line in prompt.splitlines():
        if "天使恩泽将在" not in line:
            continue
        pts = re.findall(r"\[(\d+)\s*,\s*(\d+)\]", line.split("出现并存在")[0])
        if not pts:
            continue
        dm = re.search(r"存在\s*(\d+)\s*Tick", line)
        tm = re.search(r"（Tick\s*(\d+)）", line)
        if not dm or not tm:
            continue
        duration = int(dm.group(1))
        start = int(tm.group(1))
        until = start + duration - 1
        if until >= current:
            out["angel"] = ([[int(a), int(b)] for a, b in pts], until)
    # 爆炸（矩形，最近一次预告）
    for line in prompt.splitlines():
        m = re.search(r"左上角\s*\[(\d+)\s*,\s*(\d+)\]\s*、右下角\s*"
                      r"\[(\d+)\s*,\s*(\d+)\]\s*的矩形区域将发生爆炸", line)
        if m:
            out["explosion"] = [int(m.group(1)), int(m.group(2)),
                                int(m.group(3)), int(m.group(4))]
    # 幻术（矩形）
    for line in prompt.splitlines():
        m = re.search(r"矩形\s*\[(\d+)\s*,\s*(\d+)\]\s*至\s*"
                      r"\[(\d+)\s*,\s*(\d+)\]\s*内战车将中幻术", line)
        if m:
            out["illusion"] = [int(m.group(1)), int(m.group(2)),
                               int(m.group(3)), int(m.group(4))]
    # 世界分裂（交点；公告原文为"以 {point} 为交点的整行整列…"）
    for line in prompt.splitlines():
        m = re.search(r"以\s*\[(\d+)\s*,\s*(\d+)\]\s*为交点", line)
        if m:
            out["split"] = [int(m.group(1)), int(m.group(2))]
    return out


def build_content(prompt):
    a = extract_announces(prompt)
    goals = []
    for m in re.finditer(r"type=(\w+)", prompt):
        if m.group(1) not in goals:
            goals.append(m.group(1))
    if not goals:
        goals = ["normal_resources", "high_resources", "reach"]
    plan = None
    if a["angel"]:
        pts, until = a["angel"]
        plan = {"points": pts, "avoid": False, "until_tick": until,
                "reason": "天使恩泽回血点，低血时前往可获得持续回血增益"}
    elif a["explosion"]:
        plan = {"rect": [[a["explosion"][0], a["explosion"][1]],
                         [a["explosion"][2], a["explosion"][3]]],
                "avoid": True, "until_tick": 999999,
                "reason": "爆炸预告矩形，避开该区域"}
    elif a["illusion"]:
        plan = {"rect": [[a["illusion"][0], a["illusion"][1]],
                         [a["illusion"][2], a["illusion"][3]]],
                "avoid": True, "until_tick": 999999,
                "reason": "幻术区域，避开（移动方向颠倒）"}
    elif a["split"]:
        plan = {"points": [[a["split"][0], a["split"][1]]],
                "avoid": True, "until_tick": 999999,
                "reason": "世界分裂交点，避开该行该列"}
    return {"goals": goals, "event_plan": plan,
            "summary": "mock 事件理解（模拟官方大模型）"}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # 精简日志
        pass

    def do_POST(self):
        if self.path.rstrip("/") != "/v1/chat/completions":
            self.send_response(404)
            self.end_headers()
            return
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or b"{}")
        msgs = body.get("messages", [])
        prompt = "\n".join(m.get("content", "") for m in msgs
                           if isinstance(m, dict) and m.get("role") == "user")
        content = build_content(prompt)
        print("[mock_llm] %s" % json.dumps(content, ensure_ascii=False),
              flush=True)
        resp = {
            "id": "chatcmpl-mock-selfplay",
            "object": "chat.completion",
            "created": 0,
            "model": "mock-selfplay",
            "choices": [{
                "index": 0,
                "finish_reason": "stop",
                "message": {"role": "assistant",
                            "content": json.dumps(content, ensure_ascii=False)},
            }],
            "usage": {"prompt_tokens": 0, "completion_tokens": 0,
                      "total_tokens": 0},
        }
        data = json.dumps(resp).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    print("[mock_llm] listening on 127.0.0.1:%d" % PORT, flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
