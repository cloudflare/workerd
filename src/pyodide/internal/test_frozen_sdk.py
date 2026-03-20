"""Test that the in-tree Python SDK (internal/workers-api/) is frozen.

The Python SDK now lives in https://github.com/cloudflare/workers-py and is
installed from PyPI. The copy in this repository is kept only for backward
compatibility and MUST NOT be modified.

If this test fails, it means a file under internal/workers-api/ was changed.
New features should go to cloudflare/workers-py instead. If the change is a
deliberate backward-compatibility fix, update EXPECTED_HASHES below.
"""

import hashlib
import unittest
from pathlib import Path

SDK_DIR = Path(__file__).parent / "workers-api"

# SHA-256 hashes of every file in the frozen SDK, keyed by path relative to
# internal/workers-api/.
# Calculate with: find internal/workers-api -type f -exec sha256sum {} \;
EXPECTED_HASHES = {
    "pyproject.toml": "2ba30eeea93f2cf161fce735981b382d2ca1f5aee77e663f447743aabe8575cf",
    "src/asgi.py": "da171340aa361f733d99d4a1e09c7fe440dd6c79fbca83e4f11d7c42f7622549",
    "src/workers/__init__.py": "5db8f21adacc3ba625c8763c6e8c47220325109bdc4ec301d76925037844cfd7",
    "src/workers/_workers.py": "a1e2d9b8199e4bb88c3e89e82dca037772d223bf58bcb1a241d9f03bc54282bd",
    "src/workers/workflows.py": "9379fdf416da56d2400369165a51b72da83f724aea893ac785285631d09803bf",
    "uv.lock": "8945fab16bffb1ea1fe5740ca677e40bf8fe28010325e4c389cd11b8a072a9fc",
}


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class TestFrozenSDK(unittest.TestCase):
    def test_no_files_modified(self):
        """Ensure no existing SDK files were modified."""
        for rel_path, expected_hash in sorted(EXPECTED_HASHES.items()):
            file_path = SDK_DIR / rel_path
            if not file_path.exists():
                continue
            actual_hash = _sha256(file_path)
            self.assertEqual(
                actual_hash,
                expected_hash,
                f"Python SDK is frozen — {rel_path} was modified. "
                f"New features belong in cloudflare/workers-py. "
                f"If this is a deliberate backward-compat fix, update "
                f"EXPECTED_HASHES in this test.",
            )


if __name__ == "__main__":
    unittest.main()
