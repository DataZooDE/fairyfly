"""Offline contract checks for the two-account live isolation probe."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import unittest


SCRIPT = Path(__file__).with_name("mcp_cross_account_isolation.ps1")


class OwnerEndpoint(BaseHTTPRequestHandler):
    def do_POST(self):
        token = self.headers.get("Authorization", "")
        if token not in (f"Bearer token-{self.server.owner}",
                         f"Bearer token-{self.server.owner}-attach"):
            self.reply(401, {"error_code": "AUTH_REQUIRED"})
            return
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        tool = request["params"]["name"]
        connection = request["params"]["arguments"].get("connection")
        if tool == "gui_session_attach":
            if self.server.attach_side_effect:
                (self.server.cache_dir / "fairyfly.99.con").write_text("foreign saved connection")
            if self.server.attach_leak:
                self.reply(200, {"jsonrpc": "2.0", "id": 1, "result": {
                    "content": [{"type": "text", "text": "attached"}], "isError": False}})
            else:
                self.reply(200, {"jsonrpc": "2.0", "id": 1, "result": {
                    "content": [{"type": "text", "text": self.server.attach_error_code}], "isError": True}})
            return
        if tool == "gui_session_list":
            self.reply(200, {"jsonrpc": "2.0", "id": 1, "result": {
                "content": [{"type": "text", "text": "SAP sessions"}], "isError": False,
                "structuredContent": {"data": {"total_sessions": 1, "connections": [
                    {"sessions": [{"id": self.server.session_id}]}]}}}})
            return
        elif tool == "gui_connection_list":
            self.reply(200, {"jsonrpc": "2.0", "id": 1, "result": {
                "content": [{"type": "text", "text": "OWNER_IDENTITY_UNKNOWN"}], "isError": True}})
            return
        elif tool == "gui_screen_read":
            value = (f"screen marker-{self.server.owner}" if connection == self.server.own_connection
                     else self.server.guessed_response)
        else:
            self.reply(200, {"error": {"code": -32601}})
            return
        self.reply(200, {"jsonrpc": "2.0", "id": 1, "result": {
            "content": [{"type": "text", "text": value}], "isError": False}})

    def reply(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *_args):
        pass


class IsolationProbeTests(unittest.TestCase):
    def test_remote_http_is_refused_before_token_use(self):
        result = subprocess.run([
            "powershell.exe", "-NoProfile", "-File", str(SCRIPT),
            "-EndpointA", "http://example.invalid:8383/mcp",
            "-EndpointB", "http://127.0.0.1:8384/mcp",
            "-ConnectionA", "1", "-ConnectionB", "2",
            "-SessionIdA", "/app/con[0]/ses[0]",
            "-SessionIdB", "/app/con[1]/ses[0]",
            "-ScreenMarkerA", "marker-A", "-ScreenMarkerB", "marker-B",
            "-ProtocolOnly",
        ], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("loopback", result.stderr)

    def test_live_mode_requires_process_proof(self):
        result = subprocess.run([
            "powershell.exe", "-NoProfile", "-File", str(SCRIPT),
            "-EndpointA", "http://127.0.0.1:8383/mcp",
            "-EndpointB", "http://127.0.0.1:8384/mcp",
            "-ConnectionA", "1", "-ConnectionB", "2",
            "-SessionIdA", "/app/con[0]/ses[0]",
            "-SessionIdB", "/app/con[1]/ses[0]",
            "-ScreenMarkerA", "marker-A", "-ScreenMarkerB", "marker-B",
            "-FairyflyExe", str(SCRIPT),
        ], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("process IDs", result.stderr)

    def run_probe(self, guessed_a, guessed_b, attach_leak=False,
                  attach_error_code="OWNER_IDENTITY_UNKNOWN", attach_side_effect=False,
                  with_cache=False):
        servers = []
        temp = tempfile.TemporaryDirectory()
        try:
            cache_root = Path(temp.name)
            for owner in ("A", "B"):
                server = ThreadingHTTPServer(("127.0.0.1", 0), OwnerEndpoint)
                server.owner = owner
                server.own_connection = 1 if owner == "A" else 2
                server.session_id = f"/app/con[{0 if owner == 'A' else 1}]/ses[0]"
                server.guessed_response = guessed_a if owner == "A" else guessed_b
                server.attach_leak = attach_leak
                server.attach_error_code = attach_error_code
                server.attach_side_effect = attach_side_effect
                server.cache_dir = cache_root / owner
                server.cache_dir.mkdir()
                (server.cache_dir / "fairyfly.1.con").write_text("own saved connection")
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                servers.append(server)
            env = os.environ.copy()
            env["FAIRYFLY_OWNER_A_TOKEN"] = "token-A"
            env["FAIRYFLY_OWNER_B_TOKEN"] = "token-B"
            env["FAIRYFLY_OWNER_A_ATTACH_TOKEN"] = "token-A-attach"
            env["FAIRYFLY_OWNER_B_ATTACH_TOKEN"] = "token-B-attach"
            command = [
                "powershell.exe", "-NoProfile", "-File", str(SCRIPT),
                "-EndpointA", f"http://127.0.0.1:{servers[0].server_port}/mcp",
                "-EndpointB", f"http://127.0.0.1:{servers[1].server_port}/mcp",
                "-ConnectionA", "1", "-ConnectionB", "2",
                "-SessionIdA", "/app/con[0]/ses[0]",
                "-SessionIdB", "/app/con[1]/ses[0]",
                "-ScreenMarkerA", "marker-A", "-ScreenMarkerB", "marker-B",
                "-ProtocolOnly",
            ]
            if with_cache:
                command += ["-CacheDirA", str(cache_root / "A"),
                            "-CacheDirB", str(cache_root / "B")]
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=15)
            return result
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()
            temp.cleanup()

    def test_unexpected_guessed_window_fails(self):
        result = self.run_probe("unexpected third window", "screen marker-B")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("unexpected window", result.stderr)

    def test_isolated_windows_pass(self):
        result = self.run_probe("screen marker-A", "screen marker-B", with_cache=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)["protocol_checks_passed"])
        self.assertFalse(json.loads(result.stdout)["account_endpoints_isolated"])
        self.assertTrue(json.loads(result.stdout)["attach_cache_unchanged"])

    def test_foreign_attach_success_fails(self):
        result = self.run_probe("screen marker-A", "screen marker-B", attach_leak=True)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("foreign session attach", result.stderr)

    def test_scope_denial_cannot_pass_as_owner_denial(self):
        result = self.run_probe("screen marker-A", "screen marker-B",
                                attach_error_code="SCOPE_DENIED")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("foreign session attach", result.stderr)

    def test_error_after_saved_connection_side_effect_fails(self):
        result = self.run_probe("screen marker-A", "screen marker-B",
                                attach_side_effect=True, with_cache=True)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("saved connection changed", result.stderr)


if __name__ == "__main__":
    unittest.main()
