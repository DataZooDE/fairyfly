"""Offline regression tests for the live SM59 integration runner."""

import importlib.util
import unittest
from pathlib import Path
from unittest.mock import patch


RUNNER_PATH = Path(__file__).with_name("test_integration.py")
SPEC = importlib.util.spec_from_file_location("sm59_runner", RUNNER_PATH)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


def response(status="success", data=None, exit_code=0):
    import json

    return {
        "output": json.dumps({"status": status, "data": data or {}, "error": {"message": "rejected"}}),
        "error": "",
        "exit_code": exit_code,
        "success": exit_code == 0,
    }


def capture_response(args):
    image_path = Path(args[args.index("--file") + 1]).resolve()
    image_path.write_bytes(b"\x89PNG\r\n\x1a\nmock")
    return response(data={"filepath": str(image_path)})


def command_of(args):
    return args[2] if args[:2] == ["--output", "markdown"] else args[0]


class Sm59RunnerSafetyTests(unittest.TestCase):
    def test_cli_invocation_uses_json_and_error_log_defaults(self):
        completed = runner.subprocess.CompletedProcess(
            args=[], returncode=0, stdout='{"status":"success"}', stderr="")
        with patch.object(runner.subprocess, "run", return_value=completed) as run:
            self.assertTrue(runner.run_fairyfly(["list"])["success"])
        self.assertEqual(run.call_args.args[0], [str(runner.FAIRYFLY_EXE), "list"])

    def test_unknown_existing_connection_id_stops_without_launch(self):
        calls = []

        def fake_cli(args):
            calls.append(args)
            if command_of(args) == "list":
                return response(data={"connections": []})
            if command_of(args) == "connections":
                return response(data={"connections": [{"id": 1}]})
            self.fail(f"Unexpected CLI command: {args}")

        runner.tests_passed = 0
        runner.tests_failed = 0
        with patch.object(runner, "run_fairyfly", side_effect=fake_cli):
            exit_code = runner.main(["--existing-connection-id", "0"])

        self.assertEqual(exit_code, 2)
        self.assertEqual([command_of(args) for args in calls], ["list", "connections"])

    def test_existing_connection_runs_sm59_without_launch_or_disconnect(self):
        calls = []

        def fake_cli(args):
            calls.append(args)
            command = command_of(args)
            if command == "list":
                return response(data={"connections": [{"description": "Bigfox", "backend_scripting_disabled": False}]})
            if command == "connections":
                return response(data={"connections": [{"id": 0}]})
            if command == "tcode":
                return response(data={"transaction": "SM59"})
            if command == "screen":
                if "capture" in args:
                    return capture_response(args)
                if args[:2] == ["--output", "markdown"]:
                    return {"output": "# SM59", "error": "", "exit_code": 0, "success": True}
                return response(data={"screen_id": "wnd[0]"})
            self.fail(f"Unexpected CLI command: {args}")

        runner.tests_passed = 0
        runner.tests_failed = 0
        with patch.object(runner, "run_fairyfly", side_effect=fake_cli):
            exit_code = runner.main(["--existing-connection-id", "0"])

        self.assertEqual(exit_code, 0)
        self.assertEqual([args for args in calls if "launch" in args or "disconnect" in args], [])
        self.assertEqual(
            [args for args in calls if "tcode" in args],
            [["tcode", "SM59", "--connection", "0"]],
        )

    def test_failed_transaction_skips_screen_reads_and_cleans_own_connection(self):
        calls = []

        def fake_cli(args):
            calls.append(args)
            command = command_of(args)
            if command == "list":
                return response(data={"connections": []})
            if command == "connections":
                return response(data={"connections": []})
            if command == "launch":
                return response(data={"connection_name": "Bigfox", "connection_file_id": 42})
            if command == "tcode":
                return response(status="error", exit_code=1)
            if command == "disconnect":
                return response(data={"session_closed": True, "file_deleted": True})
            return response(status="error", exit_code=1)

        runner.tests_passed = 0
        runner.tests_failed = 0
        with patch.object(runner, "run_fairyfly", side_effect=fake_cli):
            exit_code = runner.main()

        self.assertEqual(exit_code, 1)
        self.assertEqual([args for args in calls if "screen" in args], [])
        self.assertEqual(
            [args for args in calls if "disconnect" in args],
            [["disconnect", "--connection", "42", "--close-session"]],
        )

    def test_authenticated_launch_logs_in_before_sm59(self):
        calls = []

        def fake_cli(args):
            calls.append(args)
            command = command_of(args)
            if command in ("list", "connections"):
                return response(data={"connections": []})
            if command == "launch":
                return response(data={"connection_name": "Bigfox", "connection_file_id": 42,
                                      "session_id": "/app/con[0]/ses[0]"})
            if command == "login":
                return response(data={"transaction": "SESSION_MANAGER"})
            if command == "tcode":
                return response(data={"transaction": "SM59"})
            if command == "screen":
                if "capture" in args:
                    return capture_response(args)
                return ({"output": "# SM59", "error": "", "exit_code": 0, "success": True}
                        if args[:2] == ["--output", "markdown"] else response(data={"screen_id": "wnd[0]"}))
            if command == "disconnect":
                return response(data={"session_closed": True, "file_deleted": True})
            self.fail(f"Unexpected CLI command: {args}")

        runner.tests_passed = 0
        runner.tests_failed = 0
        with patch.object(runner, "run_fairyfly", side_effect=fake_cli), \
             patch.object(runner.Path, "is_file", return_value=True):
            exit_code = runner.main(["--login-from-trial-env"])

        self.assertEqual(exit_code, 0)
        self.assertEqual(len([args for args in calls if "login" in args]), 1)
        self.assertLess(next(i for i, args in enumerate(calls) if "launch" in args),
                        next(i for i, args in enumerate(calls) if "login" in args))
        self.assertLess(next(i for i, args in enumerate(calls) if "login" in args),
                        next(i for i, args in enumerate(calls) if "tcode" in args))

    def test_failed_authenticated_login_closes_owned_session(self):
        calls = []

        def fake_cli(args):
            calls.append(args)
            command = command_of(args)
            if command in ("list", "connections"):
                return response(data={"connections": []})
            if command == "launch":
                return response(data={"connection_name": "Bigfox", "connection_file_id": 42,
                                      "session_id": "/app/con[0]/ses[0]"})
            if command == "login":
                return response(status="error")
            if command == "disconnect":
                return response(data={"session_closed": True, "file_deleted": True})
            self.fail(f"Unexpected CLI command after login failure: {args}")

        runner.tests_passed = 0
        runner.tests_failed = 0
        with patch.object(runner, "run_fairyfly", side_effect=fake_cli), \
             patch.object(runner.Path, "is_file", return_value=True):
            exit_code = runner.main(["--login-from-trial-env"])

        self.assertEqual(exit_code, 1)
        self.assertFalse(any("tcode" in args or "screen" in args for args in calls))
        self.assertEqual([args for args in calls if "disconnect" in args],
                         [["disconnect", "--connection", "42", "--close-session"]])


if __name__ == "__main__":
    unittest.main()
