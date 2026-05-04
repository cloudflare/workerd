# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0


async def test(ctrl, env, ctx):
    # _pyodide_core.trigger_fatal_error() invokes Pyodide's on_fatal handler. With the
    # python-abort-isolate-on-fatal-error autogate enabled, the on_fatal handler calls
    # abortIsolate() which terminates the workerd process.
    from _pyodide_core import trigger_fatal_error

    trigger_fatal_error()
