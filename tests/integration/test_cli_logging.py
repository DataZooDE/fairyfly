"""Observable logging behavior of the built Fairyfly CLI."""

import json
import os
import subprocess
import unittest
from pathlib import Path


CLI = Path(__file__).resolve().parents[2] / "build" / "bin" / "Release" / "fairyfly.exe"


@unittest.skipUnless(CLI.is_file(), "Build the Release CLI first")
class CliLoggingTests(unittest.TestCase):
    def test_error_environment_override_emits_no_info_message(self):
        environment = os.environ.copy()
        environment["FFLYLOG_LEVEL"] = "error"
        process = subprocess.run(
            [str(CLI), "session", "list"], capture_output=True, text=True,
            encoding="utf-8", errors="replace", env=environment, timeout=30,
        )
        self.assertEqual(process.returncode, 0)
        self.assertEqual(json.loads(process.stdout)["status"], "success")
        self.assertNotIn("Using log level from FFLYLOG_LEVEL", process.stderr)


if __name__ == "__main__":
    unittest.main()
