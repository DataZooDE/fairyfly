"""Preflight checks for the disposable SAP user's MCP gate."""

from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import subprocess
import threading
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("mcp_live_disposable_user_gate.ps1")
QUEUED_CLOSE = Path(__file__).with_name("mcp_live_queued_close.ps1")


class DisposableUserGateTests(unittest.TestCase):
    def test_invalid_user_is_refused_before_any_sap_or_token_call(self):
        result = subprocess.run([
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
            "-Username", "INVALID-LONG-USER", "-CredentialName", "FFTEST-INVALID",
            "-AdminConnectionId", "7", "-ConnectionName", "Bigfox", "-FairyflyPath", str(SCRIPT),
        ], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Invalid disposable SAP user", result.stderr)

    def test_queued_close_refuses_remote_endpoint_before_sap_access(self):
        result = subprocess.run([
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(QUEUED_CLOSE),
            "-ConnectionId", "7", "-Bearer", "dummy", "-Endpoint", "http://example.com/mcp",
            "-Fairyfly", str(QUEUED_CLOSE), "-OwnMarker", "SAP Easy Access", "-OtherMarker", "SU01",
        ], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("loopback endpoint", result.stderr)

    def test_queued_close_rejects_progress_without_a_real_batch_item(self):
        class InvalidBatch(BaseHTTPRequestHandler):
            def do_POST(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                # A failed batch may emit its final 'finished' progress at 1.
                self.wfile.write(b'data: {"jsonrpc":"2.0","method":"notifications/progress","params":{"progress":1,"message":"finished"}}\n\n')
                self.wfile.flush()

            def log_message(self, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), InvalidBatch)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                fake = Path(directory) / "fairyfly-fake.ps1"
                fake.write_text("""
$global:LASTEXITCODE = 0
'{"data":{"connections":[{"id":7,"valid":true,"session_id":"/app/con[0]/ses[0]"}]}}'
""", encoding="utf-8")
                result = subprocess.run([
                    "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(QUEUED_CLOSE),
                    "-ConnectionId", "7", "-Bearer", "dummy",
                    "-Endpoint", f"http://127.0.0.1:{server.server_port}/mcp",
                    "-Fairyfly", str(fake), "-OwnMarker", "SAP Easy Access",
                    "-OtherMarker", "SU01",
                ], capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("before its first item confirmed execution", result.stderr)
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    unittest.main()
