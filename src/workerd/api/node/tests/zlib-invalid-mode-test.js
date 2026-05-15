// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-365:
// ZlibStream::constructor did not validate the mode byte.
// An invalid mode caused KJ_UNREACHABLE in initializeZlib()
// after writeStream() set writing=true, leaving the flag stuck.
// The destructor's JSG_ASSERT(!writing) then threw inside the
// noexcept cppgc finalizer, triggering std::terminate().

import assert from 'node:assert';
import { createDeflate } from 'node:zlib';

export const zlibInvalidModeIsRejected = {
  test() {
    const ZlibStream = createDeflate()._handle.constructor;

    // Mode 0 (ZlibMode::NONE) is not a valid zlib mode.
    assert.throws(() => new ZlibStream(0), {
      name: 'TypeError',
      message: /Invalid zlib mode/,
    });

    // Mode 255 is well outside the DEFLATE..UNZIP range.
    assert.throws(() => new ZlibStream(255), {
      name: 'TypeError',
      message: /Invalid zlib mode/,
    });

    // Mode 8 (BROTLI_DECODE) is not valid for ZlibStream.
    assert.throws(() => new ZlibStream(8), {
      name: 'TypeError',
      message: /Invalid zlib mode/,
    });

    // Valid modes (1=DEFLATE through 7=UNZIP) should work.
    const validStream = new ZlibStream(1); // DEFLATE
    assert.ok(validStream);
  },
};

export const zlibInvalidModeCrash = {
  test() {
    const ZlibStream = createDeflate()._handle.constructor;

    // Construct a ZlibStream with mode 0 (ZlibMode::NONE), which is invalid.
    // With the constructor fix, this throws TypeError immediately — that's fine.
    let stream;
    try {
      stream = new ZlibStream(0);
    } catch (_e) {
      // Constructor correctly rejected the invalid mode. Nothing left to test.
      return;
    }

    // If we reach here, the constructor did NOT validate the mode (pre-fix code).
    // Exercise the write path to demonstrate the crash:
    // writeSync sets writing=true, then context()->work() calls initializeZlib()
    // which hits KJ_UNREACHABLE for mode NONE. Without KJ_ON_SCOPE_FAILURE,
    // writing stays permanently true. When V8 GC later collects this object,
    // ~CompressionStream hits JSG_ASSERT(!writing) inside noexcept ~CppgcShim
    // -> std::terminate().
    const writeState = new Uint32Array(2);
    stream.initialize(15, 6, 8, 0, writeState, () => {});

    try {
      const input = new Uint8Array(1);
      const output = new Uint8Array(1024);
      stream.writeSync(0, input, 0, 1, output, 0, 1024);
    } catch (_e) {
      // Expected: KJ_UNREACHABLE throws, but writing is now stuck true.
    }

    // Dropping all references and forcing GC should trigger the crash.
    // The process should terminate here due to std::terminate().
  },
};
