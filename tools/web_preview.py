"""Local UI smoke-test fixture. Does not emulate the ESP32 Wi-Fi or HTTP stack."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
from urllib.parse import parse_qs, urlparse

PAGE = Path(__file__).resolve().parents[1] / "main/web/index.html"
DATA = "[本地界面测试数据]\nCQ CQ DE ESP32-S3\n接收内容可在手机查看。\n<script>alert('RX')</script>\n".encode() + bytes([0, 255])
state = {"mode": "BPSK500", "tone_hz": 1500}


class Handler(BaseHTTPRequestHandler):
    def send(self, data, kind, headers=None, status=200):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        for name, value in (headers or {}).items():
            self.send_header(name, str(value))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/":
            self.send(PAGE.read_bytes(), "text/html; charset=utf-8")
        elif url.path == "/api/modes":
            self.send(json.dumps([{"name": n, "enabled": True} for n in ["BPSK500", "8PSK1200F", "FT8"]]).encode(), "application/json")
        elif url.path == "/api/status":
            status = dict(state, baud=500, coded_payload_bps=500, raw_bps=500, rms=2500,
                clips=0, sample_rate=16000, work_us=210000, budget_us=1000000,
                peak_work_us=2800, overruns=0, dma_overflows=0, read_errors=0,
                output_drops=0, decode_drops=0, lcd_drops=0, lock_state=3,
                received_bytes=len(DATA), session=7, uptime_s=30)
            self.send(json.dumps(status).encode(), "application/json")
        elif url.path == "/api/rx":
            q = parse_qs(url.query)
            offset = int(q.get("since", [0])[0]) if q.get("session", ["0"])[0] == "7" else 0
            self.send(DATA[offset:], "application/octet-stream",
                {"X-RX-Next": len(DATA), "X-RX-Session": 7, "X-RX-Lost": 0})
        else:
            self.send(b"not found", "text/plain", status=404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode()
        key, value = body.split("=", 1)
        if key == "mode": state["mode"] = value
        if key == "tone": state["tone_hz"] = int(value)
        self.send(b"queued", "text/plain", status=202)

    def log_message(self, *args): pass


if __name__ == "__main__":
    ThreadingHTTPServer.allow_reuse_address = False
    server = ThreadingHTTPServer(("127.0.0.1", 18763), Handler)
    print("Local UI fixture: http://127.0.0.1:18763/ (Ctrl-C to stop)", flush=True)
    server.serve_forever()
