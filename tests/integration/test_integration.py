#!/usr/bin/env python3
"""
Simple integration tests for fairyfly CLI
Tests execute real fairyfly.exe against real SAP GUI connections
"""

import subprocess
import sys
import json
from pathlib import Path

# Configuration
FAIRYFLY_EXE = Path(__file__).parent / ".." / ".." / "build" / "Release" / "fairyfly.exe"
CONNECTION_NAME = "Bigfox"

# Test results
tests_passed = 0
tests_failed = 0


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

        # Extract JSON from output (may contain log lines before JSON)
        output = result.stdout
        json_output = output

        # If output format is JSON, find the JSON object
        if '--output' in args and 'json' in args:
            # Find first { character (start of JSON)
            json_start = output.find('{')
            if json_start != -1:
                json_output = output[json_start:]

        return {
            'output': json_output,
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


def main():
    """Run integration tests"""
    print()
    print("=" * 64)
    print("  Fairyfly Integration Tests - Real SAP GUI")
    print(f"  Connection: {CONNECTION_NAME}")
    print("=" * 64)
    print()

    # Test 1: Launch connection
    print("Test Suite 1: Connection Management")
    print()

    result = run_fairyfly(["--output", "json", "launch", CONNECTION_NAME])

    if result['success']:
        try:
            data = json.loads(result['output'])
            passed = (data.get('status') == 'success' and
                     data.get('data', {}).get('connection_name') == CONNECTION_NAME)

            if passed:
                conn_id = data.get('data', {}).get('connection_id', 'unknown')
                test_result("Launch connection", True, f"Connection ID: {conn_id}")
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

    print()

    # Test 2: Execute transaction
    print("Test Suite 2: Transaction Execution")
    print()

    result = run_fairyfly(["--output", "json", "tcode", "SM59"])

    if result['success']:
        try:
            data = json.loads(result['output'])
            passed = data.get('status') == 'success'

            if passed:
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

    # Test 3: Read screen as JSON
    print("Test Suite 3: Screen Data Extraction")
    print()

    result = run_fairyfly(["--output", "json", "screen", "read"])

    if result['success']:
        try:
            data = json.loads(result['output'])
            passed = (data.get('status') == 'success' and
                     'screen_id' in data.get('data', {}))

            if passed:
                # Save screen data
                with open('test_screen_sm59.json', 'w', encoding='utf-8') as f:
                    json.dump(data['data'], f, indent=2)

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
    result = run_fairyfly(["--output", "markdown", "screen", "read"])

    if result['success']:
        if len(result['output']) > 0:
            with open('test_screen_sm59.md', 'w', encoding='utf-8') as f:
                f.write(result['output'])

            test_result("Read screen as Markdown", True, f"Generated {len(result['output'])} bytes")
        else:
            test_result("Read screen as Markdown", False, "Empty output")
    else:
        # Try to parse error from stderr or first line of output
        error_msg = result['error'][:80] if result['error'] else f"Exit code: {result['exit_code']}"
        test_result("Read screen as Markdown", False, error_msg)

    print()

    # Test 5: Disconnect
    print("Test Suite 4: Cleanup")
    print()

    result = run_fairyfly(["--output", "json", "disconnect"])

    if result['success']:
        try:
            data = json.loads(result['output'])
            passed = data.get('status') == 'success'

            if passed:
                test_result("Disconnect", True, "Disconnected cleanly")
            else:
                test_result("Disconnect", False, f"Status: {data.get('status')}")
        except json.JSONDecodeError as e:
            test_result("Disconnect", False, f"JSON parse error: {e}")
    else:
        # Try to parse JSON error response
        try:
            error_data = json.loads(result['output'])
            error_msg = error_data.get('error', {}).get('message', 'Unknown error')
            test_result("Disconnect", False, error_msg[:80])
        except:
            msg = result['error'] if result['error'] else f"Exit code: {result['exit_code']}"
            test_result("Disconnect", False, msg)

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
    sys.exit(main())
