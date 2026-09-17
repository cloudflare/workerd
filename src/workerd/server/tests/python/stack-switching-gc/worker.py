# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# Walking every thread state's frames must not touch a suspended task's evicted
# argument stack.
#
# Regression test for https://github.com/pyodide/pyodide/issues/6464, adapted
# from test_gc_with_evicted_suspended_task in Pyodide's test_stack_switching.py.
# In workerd the fix is applied by tools/patch_pyodide_asm.ts, which makes
# saveState/restoreState detach and reattach PyThreadState.current_frame.
#
# Each time C calls into Python, _PyEval_EvalFrameDefault links an entry frame
# that lives on the wasm argument stack into the thread state's frame chain.
# When a task suspends, another task can take over its part of the argument
# stack, in which case StackState evicts the data to a copy. The incremental gc
# walks the frames of every thread state and used to follow the suspended
# task's chain into the overwritten memory.
#
# `first` suspends, `second` is placed directly below it and suspends deep in a
# chain of entry frames, then `first` resumes which evicts `second`. What ends
# up in second's old memory depends on the stack layout, so rather than hoping
# for a crash we fill the region with 0xff bytes. With the bug, the collector
# follows second's chain to its entry frame, which now consists entirely of
# 0xffffffff, and traps with a memory access out of bounds.

import asyncio
import gc

import pyodide_js
from workers import WorkerEntrypoint

from pyodide.ffi import create_proxy, run_sync


# __getattr__ is called from C, so each level puts an entry frame on the
# argument stack.
def nested(depth, then):
    if depth == 0:
        return then()
    cls = type("G", (), {"__getattr__": lambda self, name: nested(depth - 1, then)})
    return cls().x


def first(p, corrupt):
    run_sync(p)
    corrupt()
    # An incremental collection stops walking a frame chain at the first frame a
    # previous increment already visited, so an increment that happened to run
    # while second was suspended would hide the corrupted entry frame. A full
    # collection does not walk the stacks but resets the incremental collector
    # to the start of its mark phase, where the next increment walks every frame
    # of every thread state.
    gc.collect()
    gc.collect(1)
    return "first done"


def second(p, probe):
    def suspend():
        probe()
        return run_sync(p)

    return nested(4, suspend)


class Default(WorkerEntrypoint):
    async def test(self):
        # Not stored at module level because the module object can't be part of
        # the memory snapshot.
        M = pyodide_js._module

        # Stack pointer just before second suspends. All of second's entry
        # frames are above this.
        second_sp = None

        def probe():
            nonlocal second_sp
            second_sp = M.stackSave()

        def corrupt():
            # second has been evicted so nothing below first's current stack
            # pointer is live. Everything between here and second_sp used to be
            # second's data.
            sp = M.stackSave()
            assert second_sp < sp, f"unexpected stack layout: {second_sp} >= {sp}"
            M.HEAPU8.fill(0xFF, second_sp, sp)

        async def tick():
            await asyncio.sleep(0.02)

        # callPromising starts each function as its own stack-switching task
        # instead of running it on this task's stack.
        first_proxy = create_proxy(first)
        second_proxy = create_proxy(second)
        try:
            p1 = asyncio.Future()
            p2 = asyncio.Future()
            first_done = first_proxy.callPromising(p1, corrupt)
            await tick()
            second_done = second_proxy.callPromising(p2, probe)
            await tick()
            p1.set_result(None)
            first_result = await first_done
            p2.set_result("second done")
            second_result = await second_done
        finally:
            first_proxy.destroy()
            second_proxy.destroy()

        assert first_result == "first done", first_result
        assert second_result == "second done", second_result
