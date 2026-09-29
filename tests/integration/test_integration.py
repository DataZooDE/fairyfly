#!/usr/bin/env python3
"""
Simple integration tests for fairyfly CLI
Tests execute real fairyfly.exe against real SAP GUI connections
"""

import argparse
import subprocess
import sys
import json
import tempfile
from pathlib import Path

# Configuration
FAIRYFLY_EXE = Path(__file__).parent / ".." / ".." / "build" / "bin" / "Release" / "fairyfly.exe"
CONNECTION_NAME = "Bigfox"

# Test results
tests_passed = 0
tests_failed = 0


def login_from_trial_env(connection_file_id):
    """Use Fairyfly's native GUI logon; no script host or password argument."""
    repo_root = Path(__file__).resolve().parents[2]
    env_file = repo_root / "trial.env"
    if not env_file.is_file():
        return False
    result = run_fairyfly(["login", "--connection",
                           str(connection_file_id), "--credentials-file", str(env_file)])
    if not result["success"]:
        return False
    try:
        data = json.loads(result["output"])
        return data.get("status") == "success" and data.get("data", {}).get("transaction") != "S000"
    except json.JSONDecodeError:
        return False


def close_owned_connection(connection_file_id):
    result = run_fairyfly(["disconnect", "--connection",
                           str(connection_file_id), "--close-session"])
    if not result["success"]:
        return False
    try:
        data = json.loads(result["output"])
        return (data.get("status") == "success" and
                data.get("data", {}).get("session_closed") is True and
                data.get("data", {}).get("file_deleted") is True)
    except (json.JSONDecodeError, TypeError):
        return False


def run_fairyfly(args):
    """Run fairyfly command and return result"""
    cmd = [str(FAIRYFLY_EXE)] + args
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            encoding='utf-8',
            errors='replace',
            timeout=30
        )

        output = result.stdout

        return {
            'output': output,
            'raw_output': output,
            'error': result.stderr,
            'exit_code': result.returncode,
            'success': result.returncode == 0
        }
    except subprocess.TimeoutExpired:
        return {
            'output': '',
            'raw_output': '',
            'error': 'Command timeout',
            'exit_code': -1,
            'success': False
        }
    except Exception as e:
        return {
            'output': '',
            'raw_output': '',
            'error': str(e),
            'exit_code': -1,
            'success': False
        }


def test_result(name, passed, message=""):
    """Report test result"""
    global tests_passed, tests_failed

    if passed:
        print(f"  PASS: {name}")
        tests_passed += 1
    else:
        print(f"  FAIL: {name}")
        tests_failed += 1

    if message:
        print(f"        {message}")


def main(argv=()):
    """Run integration tests"""
    parser = argparse.ArgumentParser(description="Exercise SM59 through Fairyfly and SAP GUI")
    parser.add_argument(
        "--existing-connection-id", type=int, metavar="ID",
        help="Use an existing Fairyfly connection-file ID without launching or disconnecting it",
    )
    parser.add_argument(
        "--login-from-trial-env", action="store_true",
        help="Log into a newly launched SAP GUI session using local trial.env credentials",
    )
    options = parser.parse_args(argv)
    existing_connection_id = options.existing_connection_id
    if existing_connection_id is not None and existing_connection_id < 0:
        parser.error("--existing-connection-id must be nonnegative")
    if existing_connection_id is not None and options.login_from_trial_env:
        parser.error("--login-from-trial-env requires a newly launched connection")
    print()

    # A server-side scripting block makes all later SAP actions impossible.
    preflight = run_fairyfly(['list'])
    if not preflight['success']:
        print(f"Preflight failed: {preflight['error'] or preflight['output']}")
        return 2
    try:
        available = json.loads(preflight['output'])
    except json.JSONDecodeError as exc:
        print(f"Preflight returned invalid JSON: {exc}")
        return 2
    if available.get('status') != 'success':
        print(f"Preflight reported an error: {available.get('error', {})}")
        return 2
    for connection in available.get('data', {}).get('connections', []):
        if (connection.get('description') == CONNECTION_NAME
                and connection.get('backend_scripting_disabled')):
            print(f"Backend GUI scripting is disabled for {CONNECTION_NAME}; integration tests cannot run")
            return 2
    saved = run_fairyfly(['connections'])
    if not saved['success']:
        print(f"Could not inspect saved connections: {saved['error'] or saved['output']}")
        return 2
    try:
        saved_data = json.loads(saved['output'])
        if saved_data.get('status') != 'success':
            raise ValueError(saved_data.get('error', {}))
        preexisting_ids = {item['id'] for item in saved_data['data']['connections']}
    except (json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        print(f"Could not parse saved connections: {exc}")
        return 2
    if existing_connection_id is not None and existing_connection_id not in preexisting_ids:
        print(f"Connection file ID {existing_connection_id} is not present; refusing to launch another connection")
        return 2
    print("=" * 64)
    print("  Fairyfly Integration Tests - Real SAP GUI")
    print(f"  Connection: {CONNECTION_NAME}")
    print("=" * 64)
    print()

    # Test 1: Launch connection
    print("Test Suite 1: Connection Management")
    print()

    connection_file_id = existing_connection_id
    if connection_file_id is not None:
        test_result("Use existing connection", True, f"Connection file ID: {connection_file_id}")
    else:
        result = run_fairyfly(["launch", CONNECTION_NAME])

        if result['success']:
            try:
                data = json.loads(result['output'])
                connection_file_id = data.get('data', {}).get('connection_file_id')
                passed = (data.get('status') == 'success' and
                         data.get('data', {}).get('connection_name') == CONNECTION_NAME and
                         isinstance(connection_file_id, int))

                if passed:
                    test_result("Launch connection", True, f"Connection file ID: {connection_file_id}")
                else:
                    test_result("Launch connection", False, f"Invalid response: {data.get('status')}")
            except json.JSONDecodeError as e:
                test_result("Launch connection", False, f"JSON parse error: {e}")
        else:
            # Try to parse JSON error response
            try:
                error_data = json.loads(result['output'])
                error_msg = error_data.get('error', {}).get('message', 'Unknown error')
                test_result("Launch connection", False, error_msg[:80])
            except:
                msg = result['error'] if result['error'] else f"Exit code: {result['exit_code']}"
                test_result("Launch connection", False, msg)

    if connection_file_id is None or tests_failed:
        print("Stopping: no verified launched connection to target")
        return 1

    if options.login_from_trial_env:
        if not login_from_trial_env(connection_file_id):
            test_result("Submit SAP GUI login", False, "Credential submission failed")
            test_result("Close session", close_owned_connection(connection_file_id),
                        "Cleanup after failed login")
            return 1
        test_result("Submit SAP GUI login", True)

    print()

    # Test 2: Execute transaction
    print("Test Suite 2: Transaction Execution")
    print()

    result = run_fairyfly(["tcode", "SM59", "--connection", str(connection_file_id)])
    transaction_ready = False

    if result['success']:
        try:
            data = json.loads(result['output'])
            passed = data.get('status') == 'success'

            if passed:
                transaction_ready = True
                test_result("Execute SM59", True, "Transaction executed")
            else:
                test_result("Execute SM59", False, f"Status: {data.get('status')}")
        except json.JSONDecodeError as e:
            test_result("Execute SM59", False, f"JSON parse error: {e}")
    else:
        # Try to parse JSON error response
        try:
            error_data = json.loads(result['output'])
            error_msg = error_data.get('error', {}).get('message', 'Unknown error')
            test_result("Execute SM59", False, error_msg[:80])
        except:
            msg = result['error'] if result['error'] else f"Exit code: {result['exit_code']}"
            test_result("Execute SM59", False, msg)

    print()

    if transaction_ready:
        # Test 3: Read screen as JSON
        print("Test Suite 3: Screen Data Extraction")
        print()

        result = run_fairyfly(["screen", "read", "--connection", str(connection_file_id)])

        if result['success']:
            try:
                data = json.loads(result['output'])
                passed = (data.get('status') == 'success' and
                         'screen_id' in data.get('data', {}))

                if passed:
                    screen_id = data.get('data', {}).get('screen_id', 'unknown')
                    test_result("Read screen as JSON", True, f"Screen ID: {screen_id}")
                else:
                    test_result("Read screen as JSON", False, f"Status: {data.get('status')}")
            except json.JSONDecodeError as e:
                test_result("Read screen as JSON", False, f"JSON parse error: {e}")
        else:
            # Try to parse JSON error response
            try:
                error_data = json.loads(result['output'])
                error_msg = error_data.get('error', {}).get('message', 'Unknown error')
                test_result("Read screen as JSON", False, error_msg[:80])
            except:
                msg = result['error'] if result['error'] else f"Exit code: {result['exit_code']}"
                test_result("Read screen as JSON", False, msg)

        # Test 4: Read screen as Markdown
        result = run_fairyfly(["--output", "markdown", "screen", "read", "--connection", str(connection_file_id)])

        if result['success']:
            if len(result['output']) > 0:
                test_result("Read screen as Markdown", True, f"Read {len(result['output'])} bytes")
            else:
                test_result("Read screen as Markdown", False, "Empty output")
        else:
            # Try to parse error from stderr or first line of output
            error_msg = result['error'][:80] if result['error'] else f"Exit code: {result['exit_code']}"
            test_result("Read screen as Markdown", False, error_msg)

        print()

        # SAP GUI resolves HardCopy paths in its own process. A relative CLI
        # path must still create a PNG in the caller's working directory.
        with tempfile.TemporaryDirectory(prefix="fairyfly_capture_", dir=Path.cwd()) as temp_dir:
            image_path = Path(temp_dir) / "capture.png"
            relative_path = image_path.relative_to(Path.cwd())
            result = run_fairyfly(["screen", "capture", "--file",
                                   str(relative_path), "--connection", str(connection_file_id)])
            try:
                data = json.loads(result['output'])
                passed = (result['success'] and data.get('status') == 'success' and
                          Path(data.get('data', {}).get('filepath', '')).resolve() == image_path.resolve() and
                          image_path.is_file() and image_path.read_bytes().startswith(b'\x89PNG\r\n\x1a\n'))
                test_result("Capture relative screenshot path", passed,
                            "PNG created at the reported absolute path" if passed else "Missing or invalid PNG")
            except (json.JSONDecodeError, OSError, ValueError) as exc:
                test_result("Capture relative screenshot path", False, str(exc)[:80])

    # Test 5: Close the session launched by this runner
    print("Test Suite 4: Cleanup")
    print()

    if existing_connection_id is not None or connection_file_id in preexisting_ids:
        print("  SKIP: Close session (saved connection existed before this run)")
        result = None
    else:
        result = run_fairyfly(["disconnect", "--connection", str(connection_file_id), "--close-session"])

    if result is None:
        pass
    elif result['success']:
        try:
            data = json.loads(result['output'])
            passed = (data.get('status') == 'success' and
                      data.get('data', {}).get('session_closed') is True and
                      data.get('data', {}).get('file_deleted') is True)

            if passed:
                test_result("Close session", True, "SAP GUI session closed and saved connection removed")
            else:
                test_result("Close session", False, f"Status: {data.get('status')}")
        except json.JSONDecodeError as e:
            test_result("Close session", False, f"JSON parse error: {e}")
    else:
        # Try to parse JSON error response
        try:
            error_data = json.loads(result['output'])
            error_msg = error_data.get('error', {}).get('message', 'Unknown error')
            test_result("Close session", False, error_msg[:80])
        except:
            msg = result['error'] if result['error'] else f"Exit code: {result['exit_code']}"
            test_result("Close session", False, msg)

    print()

    # Summary
    print("=" * 64)
    print("  Test Summary")
    print("=" * 64)
    print(f"Total Tests:  {tests_passed + tests_failed}")
    print(f"Passed:       {tests_passed}")
    print(f"Failed:       {tests_failed}")
    print()

    if tests_failed == 0:
        print("ALL TESTS PASSED")
        return 0
    else:
        print("SOME TESTS FAILED")
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
