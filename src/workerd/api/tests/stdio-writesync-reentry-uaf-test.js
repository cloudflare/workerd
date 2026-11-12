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

// The tests below cover the caller's buffer rather than lineBuffer.
// StdioFile::write() logs each complete line through console.log, which
// can run user code. That code can detach or shrink the ArrayBuffer being
// written. StdioFile::write() must not read the caller's bytes after the
// first console.log call, so it copies them up front.

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
