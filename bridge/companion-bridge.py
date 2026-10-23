#!/usr/bin/env python3
"""
claude-companion bridge
=======================
Serves the dashboard and bridges local Claude Code sessions to the companion device.

What it does
------------
- Polls `claude agents --json --all` for the session roster (works out of the box).
- Optionally receives Claude Code hook events on POST /hook for instant state changes.
- Streams session state to any client (browser now, Tab5 firmware later) over SSE.
- Sends replies back into background sessions via `claude -p --resume`.
- Accepts image / audio uploads from the device to pipe to a session (camera / mic).

Design notes
------------
- Stdlib only, Python 3.8+. No pip install.
- Replies only work for *background* sessions (`claude --bg`); you cannot inject into a
  plain interactive TUI. That is a Claude Code constraint, not ours.
- Bind to your Tailscale IP so the device can reach it, and keep the token secret.

Run
---
    COMPANION_HOST=$(tailscale ip -4 | head -1) python3 companion-bridge.py
"""

import json
import os
import queue
import secrets
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

# --------------------------------------------------------------------------- config
HOST = os.environ.get("COMPANION_HOST", "127.0.0.1")
PORT = int(os.environ.get("COMPANION_PORT", "8787"))
POLL_SECONDS = float(os.environ.get("COMPANION_POLL_SECONDS", "2"))
CLAUDE_BIN = os.environ.get("CLAUDE_BIN", "claude")

ROOT = Path(__file__).resolve().parent
WEB_DIR = ROOT.parent / "web"
CONFIG_DIR = Path(os.environ.get("COMPANION_CONFIG_DIR", Path.home() / ".config" / "claude-companion"))
UPLOAD_DIR = CONFIG_DIR / "uploads"

EXT_BY_TYPE = {
    "image/jpeg": "jpg", "image/png": "png", "image/webp": "webp",
    "audio/wav": "wav", "audio/webm": "webm", "audio/mpeg": "mp3", "audio/mp4": "m4a",
}


def load_token():
    env = os.environ.get("COMPANION_TOKEN")
    if env:
        return env
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    tf = CONFIG_DIR / "token"
    if tf.exists():
        return tf.read_text().strip()
    tok = secrets.token_urlsafe(24)
    tf.write_text(tok)
    tf.chmod(0o600)
    return tok


TOKEN = load_token()


# ------------------------------------------------------------------- state mapping
def map_roster_state(x):
    return {
        "blocked": "needs_you", "waiting": "needs_you",
        "working": "working", "busy": "working", "running": "working",
        "done": "done", "completed": "done",
        "idle": "idle",
        "failed": "ended", "stopped": "ended", "error": "ended",
    }.get((x or "").lower(), "working")


def map_hook_label(x):
    return {
        "working": "working", "needs_input": "needs_you",
        "idle": "idle", "done": "done", "end": "ended",
    }.get((x or "").lower(), "")


# --------------------------------------------------------------------------- hub
class Hub:
    """Holds the session registry and fans out snapshots to SSE subscribers."""

    def __init__(self):
        self.lock = threading.Lock()
        self.sessions = {}          # sessionId -> session dict
        self.subscribers = set()    # set[queue.Queue]
        self._branch_cache = {}     # cwd -> (branch, ts)

    # ---- subscribers ----
    def subscribe(self):
        q = queue.Queue(maxsize=16)
        with self.lock:
            self.subscribers.add(q)
            q.put(self._snapshot_locked())
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.subscribers.discard(q)

    def _broadcast_locked(self):
        snap = self._snapshot_locked()
        for q in list(self.subscribers):
            try:
                q.put_nowait(snap)
            except queue.Full:
                pass

    def _snapshot_locked(self):
        order = {"needs_you": 0, "working": 1, "done": 2, "idle": 3, "ended": 4}
        items = sorted(
            self.sessions.values(),
            key=lambda s: (order.get(s["state"], 9), -s.get("lastEventAt", 0)),
        )
        counts = {}
        for s in items:
            counts[s["state"]] = counts.get(s["state"], 0) + 1
        return {"type": "snapshot", "ts": int(time.time() * 1000),
                "sessions": items, "counts": counts}

    def snapshot(self):
        with self.lock:
            return self._snapshot_locked()

    # ---- git branch, best effort, cached ----
    def branch_for(self, cwd):
        if not cwd:
            return ""
        now = time.time()
        hit = self._branch_cache.get(cwd)
        if hit and now - hit[1] < 30:
            return hit[0]
        branch = ""
        try:
            branch = subprocess.run(
                ["git", "-C", cwd, "rev-parse", "--abbrev-ref", "HEAD"],
                capture_output=True, text=True, timeout=2,
            ).stdout.strip()
        except Exception:
            branch = ""
        self._branch_cache[cwd] = (branch, now)
        return branch

    # ---- roster poll ----
    def apply_roster(self, roster):
        now_ms = int(time.time() * 1000)
        with self.lock:
            for a in roster:
                sid = a.get("sessionId") or a.get("id")
                if not sid:
                    continue
                cwd = a.get("cwd", "") or ""
                s = self.sessions.get(sid, {})
                s.update({
                    "sessionId": sid,
                    "id": a.get("id", sid[:8]),
                    "cwd": cwd,
                    "repo": os.path.basename(cwd.rstrip("/")) or cwd,
                    "branch": self.branch_for(cwd),
                    "kind": a.get("kind", ""),
                    "name": a.get("name", ""),
                    "startedAt": a.get("startedAt", 0),
                })
                # A recent hook event wins over the (slower) roster poll.
                fresh_hook = s.get("stateSource") == "hook" and now_ms - s.get("lastEventAt", 0) < 5000
                if not fresh_hook:
                    s["state"] = map_roster_state(a.get("state", ""))
                    s["stateSource"] = "roster"
                s.setdefault("lastEventAt", a.get("startedAt", 0))
                s.setdefault("line", "")
                self.sessions[sid] = s
            self._broadcast_locked()

    # ---- hook event ----
    def apply_hook(self, label, data):
        sid = data.get("session_id") or data.get("sessionId")
        if not sid:
            return
        cwd = data.get("cwd", "") or ""
        with self.lock:
            s = self.sessions.get(sid, {
                "sessionId": sid, "id": sid[:8], "cwd": cwd,
                "repo": os.path.basename(cwd.rstrip("/")), "branch": self.branch_for(cwd),
                "kind": "", "name": "", "startedAt": 0,
            })
            st = map_hook_label(label)
            if st:
                s["state"] = st
                s["stateSource"] = "hook"
            s["lastEventAt"] = int(time.time() * 1000)
            line = (data.get("message") or data.get("prompt")
                    or data.get("last_assistant_message") or "")
            if line:
                s["line"] = str(line)[:240]
            self.sessions[sid] = s
            self._broadcast_locked()


hub = Hub()


# ------------------------------------------------------------------- claude actions
def send_reply(session_id, cwd, text):
    """Queue a reply into a (background) session. Fire and forget — never blocks."""
    subprocess.Popen(
        [CLAUDE_BIN, "-p", "--resume", session_id, text],
        cwd=cwd or None,
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def attach_image(session_id, cwd, path, caption):
    # Claude Code reads image files referenced by path in a prompt.
    # TODO(verify): confirm the exact attach syntax for the installed Claude Code version.
    prompt = f"Please look at the image at {path}."
    if caption:
        prompt += f" {caption}"
    send_reply(session_id, cwd, prompt)


def poll_loop():
    while True:
        try:
            out = subprocess.run(
                [CLAUDE_BIN, "agents", "--json", "--all"],
                capture_output=True, text=True, timeout=15, stdin=subprocess.DEVNULL,
            )
            roster = json.loads(out.stdout or "[]")
            if isinstance(roster, dict):
                roster = roster.get("agents") or roster.get("sessions") or []
            hub.apply_roster(roster)
        except Exception as e:  # keep polling through transient failures
            sys.stderr.write(f"[poll] {e}\n")
        time.sleep(POLL_SECONDS)


# --------------------------------------------------------------------------- server
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[http] " + (fmt % args) + "\n")

    # ---- auth helpers ----
    def _query(self):
        return parse_qs(urlparse(self.path).query)

    def authed(self):
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer ") and secrets.compare_digest(auth[7:], TOKEN):
            return True
        tok = self._query().get("token", [""])[0]
        return bool(tok) and secrets.compare_digest(tok, TOKEN)

    def is_local(self):
        return self.client_address[0] in ("127.0.0.1", "::1")

    # ---- response helpers ----
    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_json(self):
        ln = int(self.headers.get("Content-Length", "0"))
        if not ln:
            return {}
        try:
            return json.loads(self.rfile.read(ln) or b"{}")
        except Exception:
            return {}

    # ---- routes ----
    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/":
            return self.serve_index()
        if path == "/events":
            return self.serve_sse()
        if path == "/api/sessions":
            if not self.authed():
                return self.send_json(401, {"error": "unauthorized"})
            return self.send_json(200, hub.snapshot())
        if path == "/healthz":
            return self.send_json(200, {"ok": True})
        return self.send_json(404, {"error": "not found"})

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/hook":
            # command hooks run locally; accept localhost without a token
            if not (self.is_local() or self.authed()):
                return self.send_json(401, {"error": "unauthorized"})
            data = self.read_json()
            hub.apply_hook(data.get("_label", ""), data)
            return self.send_json(200, {"ok": True})
        if not self.authed():
            return self.send_json(401, {"error": "unauthorized"})
        if path == "/api/reply":
            data = self.read_json()
            sid, text = data.get("sessionId"), data.get("text", "")
            if not sid or not text:
                return self.send_json(400, {"error": "sessionId and text required"})
            send_reply(sid, hub.sessions.get(sid, {}).get("cwd", ""), text)
            return self.send_json(200, {"queued": True})
        if path == "/api/uploads/image":
            return self.handle_upload("image")
        if path == "/api/uploads/audio":
            return self.handle_upload("audio")
        return self.send_json(404, {"error": "not found"})

    def serve_index(self):
        try:
            html = (WEB_DIR / "index.html").read_text()
        except FileNotFoundError:
            return self.send_json(500, {"error": "web/index.html missing"})
        body = html.replace("__COMPANION_TOKEN__", TOKEN).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def serve_sse(self):
        if not self.authed():
            return self.send_json(401, {"error": "unauthorized"})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        q = hub.subscribe()
        try:
            while True:
                try:
                    msg = q.get(timeout=15)
                    self.wfile.write(("data: " + json.dumps(msg) + "\n\n").encode())
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            hub.unsubscribe(q)

    def handle_upload(self, kind):
        q = self._query()
        sid = q.get("sessionId", [""])[0]
        caption = q.get("caption", [""])[0]
        ln = int(self.headers.get("Content-Length", "0"))
        blob = self.rfile.read(ln) if ln else b""
        UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
        ctype = self.headers.get("Content-Type", "").split(";")[0].strip()
        ext = EXT_BY_TYPE.get(ctype, "bin")
        fn = UPLOAD_DIR / f"{kind}-{int(time.time() * 1000)}.{ext}"
        fn.write_bytes(blob)
        s = hub.sessions.get(sid, {})
        if kind == "image" and sid:
            attach_image(sid, s.get("cwd", ""), str(fn), caption)
            return self.send_json(200, {"saved": str(fn), "attached": True})
        if kind == "audio" and sid:
            # TODO(M3): run speech-to-text here (whisper.cpp or an API), then
            #   send_reply(sid, s.get("cwd",""), transcript). For now we just store it.
            return self.send_json(200, {"saved": str(fn), "transcribed": False,
                                        "note": "speech-to-text not wired yet"})
        return self.send_json(200, {"saved": str(fn)})


def main():
    threading.Thread(target=poll_loop, daemon=True).start()
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    url = f"http://{HOST}:{PORT}"
    print("claude-companion bridge")
    print(f"  dashboard : {url}/?token={TOKEN}")
    print(f"  token     : {TOKEN}")
    print(f"  bound to  : {HOST}:{PORT}")
    if HOST in ("127.0.0.1", "localhost"):
        print("  note      : bound to localhost. Set COMPANION_HOST to your Tailscale IP")
        print("              (e.g. COMPANION_HOST=100.x.y.z) so the device can reach it.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
