// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';

// Wire format of payload:
//   0xff 0x0f        - V8 serialization header (version 15)
//   0x5c             - kHostObject tag
//   0x0a             - SERIALIZATION_TAG_NATIVE_ERROR (10)
//   0x01             - ErrorTag::ERROR (1)
//   0x5c 0x0a 0x01   - nested NativeError(ERROR) as the "message" value
//   0x22 0x00        - inner error message: empty UTF-8 string
//   0x6f 0x7b 0x00   - inner error properties: empty JS object
//   0x6f 0x7b 0x00   - outer error properties: empty JS object

const PAYLOAD = new Uint8Array([
  0xff, 0x0f, 0x5c, 0x0a, 0x01, 0x5c, 0x0a, 0x01, 0x22, 0x00, 0x6f, 0x7b, 0x00,
  0x6f, 0x7b, 0x00,
]).buffer;

export default {
  async queue(batch, env, ctx) {
    batch.ackAll();
  },

  async test(ctrl, env, ctx) {
    const result = await env.SERVICE.queue('clone-test-queue', [
      {
        id: 'clone-message-id',
        timestamp: new Date(),
        serializedBody: PAYLOAD,
        attempts: 1,
      },
    ]);
    assert.strictEqual(result.outcome, 'exception');
  },
};
