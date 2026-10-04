"""Contract checks for the two-SAP-user HTTP concurrency probe."""

from pathlib import Path
import json
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import unittest


SCRIPT = Path(__file__).with_name("mcp_live_parallel_gate.ps1")
BATCH_SCRIPT = Path(__file__).with_name("mcp_live_batch_overlap.ps1")


class OwnerEndpoint(BaseHTTPRequestHandler):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        token = self.headers.get("Authorization", "")
        if time.monotonic() < self.server.activation_deadline:
            self.reply(401, {"error_code": "TOKEN_INVALID"})
            return
        own = {"Bearer token-alice": 1, "Bearer token-bob": 2}.get(token)
        tool = request["params"]["name"]
        connection = request["params"]["arguments"].get("connection")
        if own is None:
            self.reply(401, {"error": {"data": {"code": "AUTH_REQUIRED"}}})
        elif tool == "gui_batch" and self.server.batch_mode:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.sse({"jsonrpc": "2.0", "method": "notifications/progress", "params": {"progress": 1}})
            time.sleep(0.2)
            self.sse({"jsonrpc": "2.0", "method": "notifications/progress", "params": {"progress": 20}})
            self.server.batch_progress20.set()
            time.sleep(0.55)
            self.sse({"jsonrpc": "2.0", "id": 1, "result": {"isError": False,
                "content": [{"text": "batch done"}]}})
        elif tool == "gui_connection_list":
            rows = [{"id": own}]
            if self.server.leak_discovery:
                rows.append({"id": 3 - own})
            self.reply(200, {"result": {"isError": False,
                "structuredContent": {"data": {"connections": rows}}}})
        elif tool == "gui_screen_read":
            if self.server.batch_mode:
                if connection == 1:
                    self.server.batch_progress20.wait(timeout=3)
                else:
                    time.sleep(0.1)
            elif self.server.serial_reads:
                with self.server.read_lock:
                    time.sleep(self.server.read_delays[connection])
            else:
                with self.server.owner_locks[connection]:
                    time.sleep(self.server.read_delays[connection])
            if connection != own:
                self.reply(200, {"result": {"isError": True,
                    "content": [{"text": "OWNER_SESSION_UNAVAILABLE"}]}})
            else:
                actual = 3 - own if self.server.misroute_screen else own
                self.reply(200, {"result": {"isError": False,
                    "content": [{"text": f"screen read for connection {own}, marker-{'A' if actual == 1 else 'B'}"}]}})
        else:
            self.reply(200, {"error": {"code": -32601}})

    def reply(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionAbortedError):
            pass  # negative probe may exit while another mock read is pending

    def sse(self, body):
        self.wfile.write(("data: " + json.dumps(body) + "\n\n").encode())
        self.wfile.flush()

    def log_message(self, *_args):
        pass


class ParallelGateContractTests(unittest.TestCase):
    def test_batch_probe_rejects_same_window_completion_before_final_response(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0), OwnerEndpoint)
        server.batch_mode = True
        server.activation_deadline = 0
        server.batch_progress20 = threading.Event()
        server.serial_reads = False
        server.misroute_screen = False
        server.read_delays = {1: 0.12, 2: 0.12}
        server.owner_locks = {1: threading.Lock(), 2: threading.Lock()}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                fake = Path(directory) / "fairyfly-fake.ps1"
                fake.write_text("""
if ($args[0] -eq 'mcp' -and $args[1] -eq 'token' -and $args[2] -eq 'create') {
    $global:LASTEXITCODE = 0
    $identity = $args[([array]::IndexOf($args, '--sap-identity') + 1)]
    if ($identity -eq 'A4H/001/ALICE') { '{"data":{"token":"token-alice"}}' }
    elseif ($identity -eq 'A4H/001/BOB') { '{"data":{"token":"token-bob"}}' }
} elseif ($args[0] -eq 'mcp' -and $args[1] -eq 'token' -and $args[2] -eq 'delete') {
    $global:LASTEXITCODE = 0
    '{"status":"success"}'
}
""", encoding="utf-8")
                result = subprocess.run([
                    "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(BATCH_SCRIPT),
                    "-SlowConnection", "1", "-ReaderConnection", "2",
                    "-SlowIdentity", "A4H/001/ALICE", "-ReaderIdentity", "A4H/001/BOB",
                    "-SlowMarker", "marker-A", "-ReaderMarker", "marker-B",
                    "-TokenActivationDelayMs", "0",
                    "-Endpoint", f"http://127.0.0.1:{server.server_port}/mcp", "-Fairyfly", str(fake),
                ], capture_output=True, text=True, timeout=20)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("Same-session read completed before the batch", result.stderr)
        finally:
            server.shutdown()
            server.server_close()

    def test_both_probes_refuse_remote_http_before_issuing_tokens(self):
        for script, connections in (
            (SCRIPT, ["-ConnectionA", "1", "-ConnectionB", "2"]),
            (BATCH_SCRIPT, ["-SlowConnection", "1", "-ReaderConnection", "2"]),
        ):
            with self.subTest(script=script.name):
                result = subprocess.run([
                    "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                    *connections, "-Fairyfly", str(script), "-Endpoint", "http://example.invalid/mcp",
                ], capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("loopback", result.stderr)

    def test_two_user_mode_requires_distinct_screen_markers(self):
        result = subprocess.run([
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
            "-ConnectionA", "1", "-ConnectionB", "2", "-Fairyfly", str(SCRIPT),
            "-IdentityA", "A4H/001/ALICE", "-IdentityB", "A4H/001/BOB",
        ], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("distinct screen markers", result.stderr)

    def test_batch_overlap_two_user_mode_requires_distinct_grants(self):
        result = subprocess.run([
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(BATCH_SCRIPT),
            "-SlowConnection", "1", "-ReaderConnection", "2", "-Fairyfly", str(BATCH_SCRIPT),
            "-SlowIdentity", "A4H/001/ALICE", "-ReaderIdentity", "A4H/001/ALICE",
        ], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("two distinct exact SAP identities", result.stderr)

    def test_batch_overlap_two_user_mode_requires_distinct_screen_markers(self):
        result = subprocess.run([
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(BATCH_SCRIPT),
            "-SlowConnection", "1", "-ReaderConnection", "2", "-Fairyfly", str(BATCH_SCRIPT),
            "-SlowIdentity", "A4H/001/ALICE", "-ReaderIdentity", "A4H/001/BOB",
        ], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("distinct screen markers", result.stderr)

    def test_two_user_mode_requires_two_distinct_exact_sap_identities(self):
        common = [
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
            "-ConnectionA", "1", "-ConnectionB", "2", "-Fairyfly", str(SCRIPT),
        ]
        for extra in (
            ["-IdentityA", "A4H/001/ALICE"],
            ["-IdentityA", "A4H/001/ALICE", "-IdentityB", "A4H/001/ALICE"],
            ["-IdentityA", "A4H/001/ALICE", "-IdentityB", "invalid"],
        ):
            with self.subTest(extra=extra):
                result = subprocess.run(common + extra, capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("two distinct exact SAP identities", result.stderr)

    def run_mock_probe(self, leak_discovery=False, misroute_screen=False, serial_reads=False,
                       activation_delay=0, read_delays=(0.12, 0.12)):
        server = ThreadingHTTPServer(("127.0.0.1", 0), OwnerEndpoint)
        server.leak_discovery = leak_discovery
        server.misroute_screen = misroute_screen
        server.serial_reads = serial_reads
        server.batch_mode = False
        server.activation_deadline = time.monotonic() + activation_delay
        server.read_lock = threading.Lock()
        server.owner_locks = {1: threading.Lock(), 2: threading.Lock()}
        server.read_delays = {1: read_delays[0], 2: read_delays[1]}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                fake = Path(directory) / "fairyfly-fake.ps1"
                fake.write_text("""
if ($args[0] -eq 'mcp' -and $args[1] -eq 'token' -and $args[2] -eq 'create') {
    $global:LASTEXITCODE = 0
    $identity = $args[([array]::IndexOf($args, '--sap-identity') + 1)]
    if ($identity -eq 'A4H/001/ALICE') { '{"data":{"token":"token-alice"}}' }
    elseif ($identity -eq 'A4H/001/BOB') { '{"data":{"token":"token-bob"}}' }
    else { throw 'Missing identity grant' }
} elseif ($args[0] -eq 'mcp' -and $args[1] -eq 'token' -and $args[2] -eq 'delete') {
    $global:LASTEXITCODE = 0
    '{"status":"success"}'
}
""", encoding="utf-8")
                return subprocess.run([
                    "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
                    "-ConnectionA", "1", "-ConnectionB", "2", "-CallsPerSession", "3",
                    "-IdentityA", "A4H/001/ALICE", "-IdentityB", "A4H/001/BOB",
                    "-ScreenMarkerA", "marker-A", "-ScreenMarkerB", "marker-B",
                    "-TokenActivationDelayMs", "300" if activation_delay else "0",
                    "-Endpoint", f"http://127.0.0.1:{server.server_port}/mcp", "-Fairyfly", str(fake),
                ], capture_output=True, text=True, timeout=30)
        finally:
            server.shutdown()
            server.server_close()

    def test_two_user_probe_checks_discovery_and_foreign_denial(self):
        result = self.run_mock_probe()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report["two_sap_user_boundary_checked"])
        self.assertTrue(report["all_responses_targeted_correctly"])

    def test_two_user_probe_fails_on_foreign_discovery(self):
        result = self.run_mock_probe(leak_discovery=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("did not isolate", result.stderr)

    def test_two_user_probe_fails_on_misrouted_screen_with_correct_requested_id(self):
        result = self.run_mock_probe(misroute_screen=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("wrong SAP window", result.stderr)

    def test_two_user_probe_fails_when_endpoint_serializes_distinct_windows(self):
        result = self.run_mock_probe(serial_reads=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("did not overlap", result.stderr)

    def test_two_user_probe_waits_for_token_store_refresh(self):
        result = self.run_mock_probe(activation_delay=0.2)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_two_user_probe_accepts_overlap_with_unequal_screen_costs(self):
        result = self.run_mock_probe(read_delays=(0.28, 0.03))
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
