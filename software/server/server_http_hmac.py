#!/usr/bin/env python3
from __future__ import annotations

import csv
import hmac
import hashlib
import json
import os
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, Optional, Tuple

import server_config as cfg


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def ensure_parent(path: str) -> None:
    parent = os.path.dirname(os.path.abspath(path))
    if parent and not os.path.exists(parent):
        os.makedirs(parent, exist_ok=True)


def ensure_csv_header_if_empty(path: str, header: list[str]) -> None:
    ensure_parent(path)
    if (not os.path.exists(path)) or os.path.getsize(path) == 0:
        with open(path, "a", newline="", encoding="utf-8") as f:
            csv.writer(f).writerow(header)


def append_row(path: str, header: list[str], row: Dict[str, Any]) -> None:
    ensure_csv_header_if_empty(path, header)
    with open(path, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=header)
        w.writerow({k: row.get(k, "") for k in header})


def bearer_token(auth_header: str) -> Optional[str]:
    parts = auth_header.strip().split(None, 1)
    if len(parts) == 2 and parts[0].lower() == "bearer":
        return parts[1].strip()
    return None


class RateLimiter:
    def __init__(self, rps: float, burst: int):
        self.rps = float(rps)
        self.burst = float(burst)
        self.lock = threading.Lock()
        self.state: Dict[str, Tuple[float, float]] = {}  # key -> (tokens, last_ts)

    def allow(self, key: str) -> bool:
        now = time.monotonic()
        with self.lock:
            tokens, last = self.state.get(key, (self.burst, now))
            tokens = min(self.burst, tokens + (now - last) * self.rps)
            if tokens < 1.0:
                self.state[key] = (tokens, now)
                return False
            self.state[key] = (tokens - 1.0, now)
            return True


ip_rl = RateLimiter(cfg.RATE_LIMIT_IP_RPS, cfg.RATE_LIMIT_IP_BURST) if cfg.RATE_LIMIT_ENABLED else None
tok_rl = RateLimiter(cfg.RATE_LIMIT_TOKEN_RPS, cfg.RATE_LIMIT_TOKEN_BURST) if cfg.RATE_LIMIT_ENABLED else None
sem = threading.BoundedSemaphore(cfg.MAX_CONCURRENT_REQUESTS)


def compute_sig(token: str, body: bytes) -> str:
    return hmac.new(token.encode("utf-8"), body, hashlib.sha256).hexdigest()


class Handler(BaseHTTPRequestHandler):
    server_version = "pico-ingest-http-hmac/1.0"

    def log_message(self, fmt: str, *args) -> None:
        if cfg.VERBOSE:
            super().log_message(fmt, *args)

    def send_json(self, code: int, obj: Dict[str, Any]) -> None:
        b = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self) -> None:
        if self.path == "/health":
            self.send_json(200, {"ok": True})
        else:
            self.send_json(404, {"ok": False, "error": "not_found"})

    def do_POST(self) -> None:
        if not sem.acquire(blocking=False):
            self.send_json(503, {"ok": False, "error": "busy"})
            return
        try:
            self._do_POST_inner()
        finally:
            sem.release()

    def _do_POST_inner(self) -> None:
        if self.path != cfg.INGEST_PATH:
            self.send_json(404, {"ok": False, "error": "not_found"})
            return

        ip = self.client_address[0]
        if ip_rl and not ip_rl.allow(ip):
            self.send_json(429, {"ok": False, "error": "rate_limited_ip"})
            return

        n = int(self.headers.get("Content-Length", "0"))
        if n <= 0:
            self.send_json(400, {"ok": False, "error": "empty_body"})
            return
        if n > cfg.MAX_BODY_BYTES:
            self.send_json(413, {"ok": False, "error": "payload_too_large"})
            return

        body = self.rfile.read(n)

        # Token lookup: prefer Bearer header so we can auth before JSON parsing
        token = None
        if cfg.ALLOW_BEARER_HEADER:
            token = bearer_token(self.headers.get("Authorization", "") or "")

        # HMAC verification (fast reject)
        if cfg.REQUIRE_HMAC:
            if not token or token not in cfg.AUTH_TOKENS:
                self.send_json(401, {"ok": False, "error": "unauthorized"})
                return

            if cfg.REQUIRE_TIMESTAMP:
                ts_s = (self.headers.get("X-Timestamp") or "").strip()
                if not ts_s.isdigit():
                    self.send_json(401, {"ok": False, "error": "missing_or_bad_timestamp"})
                    return
                ts = int(ts_s)
                now = int(time.time())
                if abs(now - ts) > cfg.TIMESTAMP_MAX_SKEW_SEC:
                    self.send_json(401, {"ok": False, "error": "timestamp_skew"})
                    return

            sig = (self.headers.get("X-Signature") or "").strip().lower()
            if len(sig) != 64:
                self.send_json(401, {"ok": False, "error": "missing_or_bad_signature"})
                return

            expected = compute_sig(token, body)
            if not hmac.compare_digest(sig, expected):
                self.send_json(401, {"ok": False, "error": "bad_signature"})
                return

        # If no Bearer header, allow legacy JSON token (but requires parsing)
        try:
            data = json.loads(body.decode("utf-8"))
        except Exception:
            self.send_json(400, {"ok": False, "error": "invalid_json"})
            return

        if not token:
            token = data.get("auth_token")

        if not token or token not in cfg.AUTH_TOKENS:
            if cfg.VERBOSE:
                print(f"[AUTH FAIL] ip={ip} token={token!r}")
            self.send_json(401, {"ok": False, "error": "unauthorized"})
            return

        if tok_rl and not tok_rl.allow(token):
            self.send_json(429, {"ok": False, "error": "rate_limited_token"})
            return

        device_id = data.get("device_id")
        if not device_id:
            self.send_json(400, {"ok": False, "error": "missing_device_id"})
            return

        row = {k: data.get(k, "") for k in cfg.CSV_HEADER}
        row["server_ts_utc"] = utc_now_iso()
        row["device_id"] = device_id

        if cfg.VERBOSE:
            print(f"[OK] device_id={device_id} token_name={cfg.AUTH_TOKENS.get(token)!r} ip={ip} ms={data.get('ms','')}")

        try:
            append_row(cfg.CSV_PATH_1, cfg.CSV_HEADER, row)
            append_row(cfg.CSV_PATH_2, cfg.CSV_HEADER, row)
        except Exception as e:
            if cfg.VERBOSE:
                print(f"[CSV ERROR] {e}")
            self.send_json(500, {"ok": False, "error": "csv_write_failed"})
            return

        self.send_json(200, {"ok": True})


def main() -> int:
    ensure_csv_header_if_empty(cfg.CSV_PATH_1, cfg.CSV_HEADER)
    ensure_csv_header_if_empty(cfg.CSV_PATH_2, cfg.CSV_HEADER)

    httpd = ThreadingHTTPServer((cfg.HOST, cfg.PORT), Handler)
    print(f"Listening on http://{cfg.HOST}:{cfg.PORT}{cfg.INGEST_PATH} (HMAC={cfg.REQUIRE_HMAC})")
    return httpd.serve_forever() or 0


if __name__ == "__main__":
    raise SystemExit(main())
