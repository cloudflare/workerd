# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# A startup that gets this far has imported pyodide-internal:internalJaeger and
# pyodide-internal:fatal-reporter, and told pyodide-internal:limiter when it began.


def test():
    print("Python Worker started with the Rust Pyodide bridge")
