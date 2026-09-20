# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

DIR = Path(__file__).parent
sys.path.insert(0, str(DIR))

from topLevelEntropy.import_patch_manager import patches
import topLevelEntropy.entropy_import_context_packages  # noqa: F401


class TestEntropyPatches(unittest.TestCase):
    def test_httpx_user_agent_patch(self):
        """Ensure httpx._transports.emscripten has HEADERS_TO_IGNORE cleared."""
        self.assertIn("httpx._transports.emscripten", patches)
        mock_httpx = SimpleNamespace(HEADERS_TO_IGNORE=("user-agent",))
        with patches["httpx._transports.emscripten"].exec(mock_httpx):
            pass
        self.assertEqual(
            mock_httpx.HEADERS_TO_IGNORE,
            (),
            "Expected HEADERS_TO_IGNORE to be cleared for httpx",
        )

    def test_httpx2_user_agent_patch(self):
        """Ensure httpx2_jsfetch has HEADERS_TO_IGNORE cleared."""
        self.assertIn("httpx2_jsfetch", patches)
        mock_httpx2 = SimpleNamespace(HEADERS_TO_IGNORE=("user-agent",))
        with patches["httpx2_jsfetch"].exec(mock_httpx2):
            pass
        self.assertEqual(
            mock_httpx2.HEADERS_TO_IGNORE,
            (),
            "Expected HEADERS_TO_IGNORE to be cleared for httpx2_jsfetch",
        )


if __name__ == "__main__":
    unittest.main()
