// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-395: heap use-after-free in
// writeStdio() via re-entrant console.log getter.
//
// writeStdio() used to capture a kj::ArrayPtr aliasing StdioFile::lineBuffer's
// heap backing store, then perform JS property lookups (console / console.log)
// that can run user-defined getters. A getter that calls fs.writeSync(1, ...)
// re-enters StdioFile::write, which can grow lineBuffer and free the old
// backing store, leaving writeStdio reading freed memory.
//
// Without the fix, this crashes under ASAN with:
//   heap-use-after-free READ at src/workerd/io/worker-fs.c++
//
// With the fix, writeStdio copies the bytes into an owned kj::String before
// any JS property access, and StdioFile::write moves lineBuffer into a local
// before calling writeStdio, so re-entrant writes cannot invalidate the
// buffer.

import * as fs from 'node:fs';
import assert from 'node:assert';

export const stdioWriteSyncReentryUafTest = {
  test() {
    const origLog = console.log;
    let armed = false;
    let getterFired = false;

    // Install a getter on console.log that re-enters StdioFile::write
    // when armed. This forces lineBuffer to grow (and potentially
    // reallocate), which would free the buffer that writeStdio() captured.
    Object.defineProperty(console, 'log', {
      configurable: true,
      get() {
        if (armed) {
          armed = false;
          getterFired = true;
          // Write a large buffer with no newline to force
          // lineBuffer.addAll() to grow, freeing the old backing store.
          const big = Buffer.alloc(4000, 0x42);
          fs.writeSync(1, big);
        }
        return origLog;
      },
    });

    try {
      // Step 1: Prime lineBuffer with non-newline data (buffered, no
      // flush).
      fs.writeSync(1, Buffer.from('AAAAA'));

      // Step 2: Arm the getter and trigger the newline path.
      // writeStdio(lineBuffer.asPtr()) -> console.get("log") runs getter
      // -> re-entrant write grows lineBuffer -> old backing store freed
      // -> writeStdio reads from owned copy (safe).
      armed = true;
      fs.writeSync(1, Buffer.from('X\n'));

      // If we get here without crashing, the fix is working.
      // Verify the getter actually fired (otherwise the test is not
      // exercising the vulnerable path).
      assert.ok(
        getterFired,
        'console.log getter should have fired during writeStdio'
      );
    } finally {
      // Restore console.log
      delete console.log;
      console.log = origLog;
    }
  },
};
