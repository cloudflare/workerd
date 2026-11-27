// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-395: a heap use-after-free
// in the former C++ stdio line buffer (StdioFile in io/worker-fs.c++), via a
// console.log getter that re-enters fs.writeSync(1, ...). Stdio is now
// line-buffered in internal_fs_stdio.ts; these tests keep re-entrant and
// mutating console.log calls covered.

import * as fs from 'node:fs';
import assert from 'node:assert';

export const stdioWriteSyncReentryUafTest = {
  test() {
    const origLog = console.log;
    let armed = false;
    let getterFired = false;

    // Install a getter on console.log that re-enters fs.writeSync when
    // armed, growing the pending partial line while a line is logged.
    Object.defineProperty(console, 'log', {
      configurable: true,
      get() {
        if (armed) {
          armed = false;
          getterFired = true;
          // Write a large buffer with no newline to grow the pending line.
          const big = Buffer.alloc(4000, 0x42);
          fs.writeSync(1, big);
        }
        return origLog;
      },
    });

    try {
      // Step 1: Prime the pending line with non-newline data.
      fs.writeSync(1, Buffer.from('AAAAA'));

      // Step 2: Arm the getter and complete the line, so that logging it
      // runs the getter.
      armed = true;
      fs.writeSync(1, Buffer.from('X\n'));

      // If we get here without crashing, the fix is working.
      // Verify the getter actually fired (otherwise the test is not
      // exercising the vulnerable path).
      assert.ok(
        getterFired,
        'console.log getter should have fired during the write'
      );
    } finally {
      // Restore console.log
      delete console.log;
      console.log = origLog;
    }
  },
};

// The tests below cover the caller's buffer rather than the pending line.
// A write logs each complete line through console.log, which can run user
// code. That code can detach or shrink the ArrayBuffer being written, so the
// write must not read the caller's bytes after the first console.log call.

// Writes `view` to stdout. The first console.log call runs `mutate`. Returns
// the lines that reached console.log.
function writeWithReentrantLog(view, mutate) {
  const origLog = console.log;
  const logged = [];
  let fired = false;
  console.log = (line) => {
    logged.push(line);
    if (!fired) {
      fired = true;
      mutate();
    }
  };
  try {
    fs.writeSync(1, view);
  } finally {
    console.log = origLog;
  }
  assert.ok(fired, 'console.log should have been called during the write');
  return logged;
}

// Enough newline-terminated lines that the write keeps going long after
// console.log first returns.
const LINE_COUNT = 64;
function fillLines(bytes) {
  const text = new TextEncoder().encode('line\n'.repeat(LINE_COUNT));
  bytes.set(text);
  return text.length;
}

function assertAllLinesLogged(logged) {
  assert.strictEqual(logged.length, LINE_COUNT);
  // Each line may carry the configured stdout prefix, such as "stdout: ".
  for (const line of logged) assert.match(line, /(^| )line$/);
}

export const stdioWriteSyncShrinkDuringLogTest = {
  test() {
    // Shrinking a resizable ArrayBuffer decommits its pages, so reading the
    // old bytes faults even without ASAN.
    const ab = new ArrayBuffer(0, { maxByteLength: 1024 * 1024 });
    ab.resize(LINE_COUNT * 'line\n'.length);
    const view = new Uint8Array(ab);
    fillLines(view);
    const logged = writeWithReentrantLog(view, () => ab.resize(0));
    assert.strictEqual(ab.byteLength, 0);
    assertAllLinesLogged(logged);
  },
};

export const stdioWriteSyncTransferDuringLogTest = {
  test() {
    const ab = new ArrayBuffer(LINE_COUNT * 'line\n'.length);
    const view = new Uint8Array(ab);
    fillLines(view);
    const logged = writeWithReentrantLog(view, () => {
      // transfer() may hand the same backing store to the new buffer, so
      // overwrite it: the write must still log the original bytes.
      const moved = new Uint8Array(ab.transfer());
      moved.fill(0x5a);
    });
    assert.ok(ab.detached);
    assertAllLinesLogged(logged);
  },
};
