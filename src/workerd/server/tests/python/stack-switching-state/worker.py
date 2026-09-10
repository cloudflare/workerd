# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

import asyncio
import contextvars
import threading

from workers import WorkerEntrypoint

_request_id = contextvars.ContextVar("request_id", default="<unset>")
_request_id.set("ROOT")

_thread_state = threading.local()
_thread_state.value = "ROOT"


class Default(WorkerEntrypoint):
    async def inner_context(self):
        prev_context_var = _request_id.get()
        _request_id.set("INNER")
        after_context_var = _request_id.get()

        prev_thread_state = _thread_state.value
        _thread_state.value = "INNER"
        after_thread_state = _thread_state.value

        return (
            prev_context_var,
            after_context_var,
            prev_thread_state,
            after_thread_state,
        )

    def test(self):
        _request_id.set("TEST")
        _thread_state.value = "TEST"

        assert _request_id.get() == "TEST"
        assert _thread_state.value == "TEST"

        inner = self.event_loop_work(self.inner_context())

        # context vars are not shared between event loops
        assert inner[0] == "TEST"
        assert inner[1] == "INNER"
        assert _request_id.get() == "TEST"
        # thread states are shared between event loops
        assert inner[2] == "TEST"
        assert inner[3] == "INNER"
        assert _thread_state.value == "INNER"

    def event_loop_work(self, foo) -> str:
        previous_loop = asyncio.get_event_loop()
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)

        try:
            return loop.run_until_complete(foo)
        finally:
            loop.close()
            asyncio.set_event_loop(previous_loop)
