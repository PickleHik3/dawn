#!/usr/bin/env python3
"""Mock of Termux Launcher's TAI: the OpenAI-compatible surface dawn's libai bridge expects.

Every request is appended to requests.log as one JSON line so a test can prove what dawn sent.
Behaviour knobs live in the Knobs class and can be flipped by POSTing to /_mock/<knob>/<value>.

The embedder behaves like EmbeddingGemma behind TAI: the server adds the prompt prefixes
("task: search result | query: " for input_type query, "title: <title or none> | text: " for a
document) before vectorising, so a query and a document of the same words differ; `dimensions`
truncates the full vector (Matryoshka) and renormalises; an input whose estimated tokens (len/4)
exceed the window is cut and reported `truncated`. Command line, all optional:

  mock_tai.py [PORT] --embed-model ID --embed-revision R --embed-dims N --embed-window N
              --embed-429-every N --embed-fail CODE --embed-no-matryoshka

--embed-429-every N answers every Nth embeddings request with 429 and Retry-After: 2.
--embed-fail CODE answers every embeddings request with 400 and that error code
(e.g. embedding_tokenizer_missing); the code "unauthorized" answers 401 instead. The same
settings are knobs (embed_model, embed_revision, embed_dims, embed_window, embed_429_every,
embed_fail, embed_no_matryoshka), so a test can swap the model while dawn runs:
  curl -X POST localhost:8765/_mock/embed_model/embeddinggemma-2-300m
"""
import argparse
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

LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "requests.log")
CHAT_ID = "mock-chat-4b"
MATRYOSHKA = (128, 256, 512, 768)
QUERY_PREFIX = "task: search result | query: "


class Knobs:
    reply_mode = "plain"  # plain | tool | edit | claim | error409 | notools | slow
    active_generation = False
    speak_seconds = 0.4
    title_reply = "good"  # good | junk: what the quiet-lane title prompt gets back
    embed_model = "embeddinggemma-300m"
    embed_revision = "r1"
    embed_dims = 768  # the model's full vector length (_endpoint_dimensions)
    embed_window = 2048  # _endpoint_context_window, in tokens
    embed_429_every = 0  # every Nth embeddings request gets 429 (0: never)
    embed_fail = ""  # every embeddings request fails with this code ("": never)
    embed_no_matryoshka = False  # list no Matryoshka sizes (dawn then sends no `dimensions`)


K = Knobs()
_lock = threading.Lock()
_embed_requests = 0


def log(kind, path, body, extra=None):
    rec = {"t": round(time.time(), 3), "kind": kind, "path": path, "body": body}
    if extra:
        rec.update(extra)
    with _lock:
        with open(LOG, "a") as f:
            f.write(json.dumps(rec) + "\n")


def vec(text, dims):
    """Deterministic unit vector: hash words into dims buckets so similar texts land near each other."""
    v = [0.0] * dims
    for w in text.lower().split():
        h = int.from_bytes(hashlib.sha1(w.encode()).digest()[:4], "little")
        v[h % dims] += 1.0
        v[(h >> 8) % dims] += 0.5
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


def matryoshka_dims():
    """The sizes a vector may be truncated to: the standard ones below the full length, and it."""
    if K.embed_no_matryoshka:
        return []
    return sorted({d for d in MATRYOSHKA if d < K.embed_dims} | {K.embed_dims})


def models():
    embedder = {"id": K.embed_model, "object": "model", "_capabilities": ["text_embeddings"],
                "_revision": K.embed_revision, "_endpoint_dimensions": K.embed_dims,
                "_endpoint_normalized": True, "_endpoint_max_batch": 16,
                "_endpoint_context_window": K.embed_window}
    if not K.embed_no_matryoshka:
        embedder["_endpoint_matryoshka_dims"] = matryoshka_dims()
    return {
        "object": "list",
        "data": [
            {"id": CHAT_ID, "object": "model", "_display_name": "Mock Chat 4B",
             "_endpoint_context_window": 8192, "_size": 2_400_000_000, "_capabilities": ["text_chat", "tool_use"]},
            {"id": "mock-tiny-1b", "object": "model", "_display_name": "Mock Tiny 1B",
             "_endpoint_context_window": 4096, "_size": 700_000_000, "_capabilities": ["text_chat", "tool_use"]},
            embedder,
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

    def _json(self, code, obj, headers=None):
        data = json.dumps(obj).encode()
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
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
            return self._json(200, models())
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
        global _embed_requests
        body = body or {}
        with _lock:
            _embed_requests += 1
            n = _embed_requests
        if K.embed_429_every > 0 and n % K.embed_429_every == 0:
            return self._json(429, {"error": {"message": "slow down", "code": "rate_limited"}},
                              {"Retry-After": "2"})
        if K.embed_fail:
            code = 401 if K.embed_fail == "unauthorized" else 400
            return self._json(code, {"error": {"message": "mock failure: " + K.embed_fail, "code": K.embed_fail}})
        if body.get("model") != K.embed_model:
            return self._json(404, {"error": {"message": "no such model: %s" % body.get("model"),
                                              "code": "model_not_found"}})
        inputs = body.get("input")
        if not isinstance(inputs, list):
            return self._json(400, {"error": {"message": "input must be an array", "code": "bad_request"}})
        dims = body.get("dimensions")
        if dims is None:
            dims = K.embed_dims
        elif not isinstance(dims, int) or dims not in matryoshka_dims():
            return self._json(400, {"error": {"message": "dimensions must be one of %s" % matryoshka_dims(),
                                              "code": "invalid_dimensions"}})
        query = body.get("input_type") == "query"
        title = body.get("title") or "none"
        fmt = body.get("encoding_format")
        data = []
        total = 0
        for i, t in enumerate(inputs):
            text = t if isinstance(t, str) else json.dumps(t)
            prompt = (QUERY_PREFIX if query else "title: %s | text: " % title) + text
            tokens = max(1, len(prompt) // 4)
            truncated = tokens > K.embed_window
            if truncated:
                prompt = prompt[:K.embed_window * 4]
                tokens = K.embed_window
            total += tokens
            # The full vector, then Matryoshka: keep the first dims values and renormalise.
            v = vec(prompt, K.embed_dims)[:dims]
            norm = math.sqrt(sum(x * x for x in v)) or 1.0
            v = [x / norm for x in v]
            if fmt == "base64":
                emb = base64.b64encode(struct.pack("<%df" % len(v), *v)).decode()
            else:
                emb = v
            data.append({"object": "embedding", "index": i, "embedding": emb,
                         "tokens": tokens, "truncated": truncated})
        return self._json(200, {"object": "list", "data": data, "model": K.embed_model,
                                "usage": {"prompt_tokens": total, "total_tokens": total}})


def parse_args(argv):
    ap = argparse.ArgumentParser(description="Mock of Termux Launcher's TAI for dawn's UX tests.")
    ap.add_argument("port", nargs="?", type=int, default=8765)
    ap.add_argument("--embed-model", default=K.embed_model, help="the embedder's id")
    ap.add_argument("--embed-revision", default=K.embed_revision, help="its _revision")
    ap.add_argument("--embed-dims", type=int, default=K.embed_dims, help="its full vector length")
    ap.add_argument("--embed-window", type=int, default=K.embed_window, help="its context window, in tokens")
    ap.add_argument("--embed-429-every", type=int, default=0, help="answer every Nth embeddings request with 429")
    ap.add_argument("--embed-fail", default="", help="answer every embeddings request with this error code")
    ap.add_argument("--embed-no-matryoshka", action="store_true", help="list no Matryoshka sizes")
    return ap.parse_args(argv)


if __name__ == "__main__":
    args = parse_args(sys.argv[1:])
    K.embed_model = args.embed_model
    K.embed_revision = args.embed_revision
    K.embed_dims = max(1, args.embed_dims)
    K.embed_window = max(1, args.embed_window)
    K.embed_429_every = max(0, args.embed_429_every)
    K.embed_fail = args.embed_fail
    K.embed_no_matryoshka = args.embed_no_matryoshka
    open(LOG, "a").close()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), H)
    print(f"mock TAI on http://127.0.0.1:{args.port}  embedder={K.embed_model}  log={LOG}", flush=True)
    srv.serve_forever()
