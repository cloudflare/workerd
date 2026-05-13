// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-110:
// Dropping the only JS reference to an unnamed WorkerStub during the getCode
// callback and forcing GC must not crash the process. Before the fix,
// synchronous destruction of WorkerStubImpl via DeleteQueue's fast path would
// destroy the start() coroutine's ChainPromiseNode while it was still firing,
// tripping KJ_REQUIRE(!firing) in Event::~Event() and aborting the process.
import assert from 'node:assert';

export let unnamedStubGcDuringGetCode = {
  async test(ctrl, env, ctx) {
    let getCodeCalled = false;
    let _stub;
    _stub = env.loader.get(null, async () => {
      getCodeCalled = true;
      // Drop the only JS reference to the unnamed stub.
      _stub = null;
      // Force V8 garbage collection so the CppgcShim destructor runs
      // synchronously on this turn, which would trigger the bug pre-fix.
      gc();
      gc();
      return {
        compatibilityDate: '2025-01-01',
        mainModule: 'main.js',
        modules: {
          'main.js': `
            import {WorkerEntrypoint} from "cloudflare:workers";
            export default class extends WorkerEntrypoint {
              ping() { return 'pong'; }
            }
          `,
        },
      };
    });

    // Yield to the event loop so the reentry callback (getCode) fires.
    // Before the fix, the process would abort here with:
    //   "Promise callback destroyed itself."
    await scheduler.wait(100);

    // If we reach this line, the process did not crash — the fix is working.
    assert.ok(getCodeCalled, 'getCode callback should have been invoked');
  },
};
