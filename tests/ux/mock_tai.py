#!/usr/bin/env python3
"""Mock of Termux Launcher's TAI: the OpenAI-compatible surface dawn's libai bridge expects.

Every request is appended to requests.log as one JSON line so a test can prove what dawn sent.
Behaviour knobs live in the Knobs class and can be flipped by POSTing to /_mock/<knob>/<value>.
"""
import base64
import hashlib
import json
import math
import os
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "requests.log")
DIMS = 256
CHAT_ID = "mock-chat-4b"
EMBED_ID = "embeddinggemma-300m"


class Knobs:
    reply_mode = "plain"  # plain | tool | edit | claim | error409 | notools | slow
    active_generation = False
    speak_seconds = 0.4
    title_reply = "good"  # good | junk: what the quiet-lane title prompt gets back


K = Knobs()
_lock = threading.Lock()


def log(kind, path, body, extra=None):
    rec = {"t": round(time.time(), 3), "kind": kind, "path": path, "body": body}
    if extra:
        rec.update(extra)
    with _lock:
        with open(LOG, "a") as f:
            f.write(json.dumps(rec) + "\n")


def vec(text):
    """Deterministic unit vector: hash words into DIMS buckets so similar texts land near each other."""
    v = [0.0] * DIMS
    for w in text.lower().split():
        h = int.from_bytes(hashlib.sha1(w.encode()).digest()[:4], "little")
        v[h % DIMS] += 1.0
        v[(h >> 8) % DIMS] += 0.5
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


MODELS = {
    "object": "list",
    "data": [
        {"id": CHAT_ID, "object": "model", "_display_name": "Mock Chat 4B",
         "_endpoint_context_window": 8192, "_size": 2_400_000_000, "_capabilities": ["text_chat", "tool_use"]},
        {"id": "mock-tiny-1b", "object": "model", "_display_name": "Mock Tiny 1B",
         "_endpoint_context_window": 4096, "_size": 700_000_000, "_capabilities": ["text_chat", "tool_use"]},
        {"id": EMBED_ID, "object": "model", "_capabilities": ["text_embeddings"], "_revision": "r1",
         "_endpoint_matryoshka_dims": [128, 256, 512, 768], "_endpoint_max_batch": 16,
         "_endpoint_context_window": 2048},
    ],
}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):  # quiet
        pass

    def _read(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw) if raw else None
        except Exception:
            return {"_raw": raw.decode("utf8", "replace")}

    def _json(self, code, obj):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _auth(self):
        return self.headers.get("Authorization")

    def do_GET(self):
        p = self.path
        log("GET", p, None, {"auth": self._auth()})
        if p == "/v1/models":
            return self._json(200, MODELS)
        if p in ("/v1/ai/runtime", "/v1/ai/status"):
            return self._json(200, {"ok": True, "runtime": {
                "loaded": True, "loadedModelId": CHAT_ID, "state": "ready",
                "activeGeneration": K.active_generation}})
        return self._json(404, {"error": {"message": "no such route", "code": "not_found"}})

    def do_POST(self):
        p = self.path
        body = self._read()
        if p.startswith("/_mock/"):
            _, _, knob, val = p.split("/", 3)
            cur = getattr(K, knob)
            setattr(K, knob, type(cur)(val) if not isinstance(cur, bool) else val == "true")
            return self._json(200, {"ok": True, knob: getattr(K, knob)})
        log("POST", p, body, {"auth": self._auth(), "accept": self.headers.get("Accept")})
        if p == "/v1/chat/completions":
            return self.chat(body)
        if p == "/v1/embeddings":
            return self.embeddings(body)
        if p == "/v1/tokenize":
            return self._json(200, {"tokens": max(1, len((body or {}).get("input", "")) // 4)})
        if p == "/v1/ai/runtime/cancel":
            return self._json(200, {"ok": True})
        if p == "/v1/ai/runtime/keep-warm":
            return self._json(200, {"ok": True, "minutes": (body or {}).get("minutes")})
        if p == "/v1/ai/speak":
            time.sleep(K.speak_seconds)
            return self._json(200, {"ok": True, "audioSeconds": K.speak_seconds, "firstSoundMs": 120,
                                    "played": True, "stopped": False})
        if p == "/v1/ai/speak/stop":
            return self._json(200, {"ok": True})
        return self._json(404, {"error": {"message": "no such route", "code": "not_found"}})

    # ---- chat -------------------------------------------------------------
    def _sse(self, chunks, usage=(42, 7)):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        for c in chunks:
            self.wfile.write(b"data: " + json.dumps(c).encode() + b"\n\n")
            self.wfile.flush()
            time.sleep(0.03)
        self.wfile.write(b"data: " + json.dumps({"choices": [], "usage": {
            "prompt_tokens": usage[0], "completion_tokens": usage[1]}}).encode() + b"\n\n")
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()
        self.close_connection = True

    def chat(self, body):
        msgs = (body or {}).get("messages", [])
        has_tools = bool((body or {}).get("tools"))
        last_user = next((m for m in reversed(msgs) if m.get("role") == "user"), {})
        last_tool = next((m for m in reversed(msgs) if m.get("role") == "tool"), None)
        user_text = last_user.get("content") or ""
        if isinstance(user_text, list):
            user_text = " ".join(x.get("text", "") for x in user_text if isinstance(x, dict))
        head = user_text.split("\n\n---\n")[0].strip()
        mode = K.reply_mode
        if mode == "error409":
            return self._json(409, {"error": {"message": "busy", "type": "server_error",
                                              "code": "generation_active"}})
        if mode == "notools" and has_tools:
            return self._json(400, {"error": {"message": "tools are not supported by this model",
                                              "code": "capability_not_supported"}})
        if mode == "tool" and has_tools and last_tool is None:
            return self._sse([{"choices": [{"index": 0, "delta": {"role": "assistant", "tool_calls": [
                {"index": 0, "id": "call_1", "type": "function",
                 "function": {"name": "get_time", "arguments": "{}"}}]}, "finish_reason": "tool_calls"}]}])
        if "Give the note a title" in user_text:
            if K.title_reply == "junk":
                text = "Mock reply to: '(From dawn, not typed by the user.) Give the note a title: two to six words', with \"quotes\" and a colon: here."
            else:
                text = "Fox Sentence Notes"
        elif mode == "claim":
            text = "I have added the task \"- [ ] buy milk\" to the note."
        elif last_tool is not None:
            text = f"The tool said: {last_tool.get('content')!s}"[:200]
        elif mode == "edit":
            text = "Adding a line for you.\n<append_to_note>\n- [ ] line appended by the mock model\n</append_to_note>"
        else:
            saw_note = "\n\n---\n" in user_text
            text = (f"Mock reply to: {head[:60]!r}. I can see the note context: {str(saw_note).lower()}. "
                    f"Tools offered: {str(has_tools).lower()}.")
        if mode == "slow":
            time.sleep(2.5)
        words = text.split(" ")
        chunks = [{"choices": [{"index": 0, "delta": {"role": "assistant", "content": ""}}]}]
        for i, w in enumerate(words):
            chunks.append({"choices": [{"index": 0, "delta": {"content": (w if i == 0 else " " + w)}}]})
        chunks[-1]["choices"][0]["finish_reason"] = "stop"
        return self._sse(chunks, usage=(len(user_text) // 4, len(words)))

    # ---- embeddings -------------------------------------------------------
    def embeddings(self, body):
        inputs = (body or {}).get("input")
        if not isinstance(inputs, list):
            return self._json(400, {"error": {"message": "input must be an array", "code": "bad_request"}})
        fmt = (body or {}).get("encoding_format")
        data = []
        for i, t in enumerate(inputs):
            v = vec(t if isinstance(t, str) else json.dumps(t))
            if fmt == "base64":
                emb = base64.b64encode(struct.pack("<%df" % DIMS, *v)).decode()
            else:
                emb = v
            data.append({"object": "embedding", "index": i, "embedding": emb,
                         "tokens": max(1, len(str(t)) // 4), "truncated": False})
        return self._json(200, {"object": "list", "data": data, "model": EMBED_ID})


if __name__ == "__main__":
    open(LOG, "a").close()
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    print(f"mock TAI on http://127.0.0.1:{PORT}  log={LOG}", flush=True)
    srv.serve_forever()
