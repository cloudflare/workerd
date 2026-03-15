// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, ok } from 'node:assert';

export const ByobReaderResizePendingRead = {
  async test() {
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    const writer = ts.writable.getWriter();

    const buffer = new ArrayBuffer(8192, { maxByteLength: 16384 });
    const view = new Uint8Array(buffer, 4096, 1024);

    const readPromise = reader.read(view);
    buffer.resize(2048);

    await writer.write(new Uint8Array(100).fill(0x41));
    const result = await readPromise;

    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 0);

    reader.releaseLock();
    writer.releaseLock();
  },
};

export const ByobReaderResizableBufferTempLifetime = {
  async test() {
    const data = 'A'.repeat(1024);
    const response = await fetch('data:text/plain;base64,' + btoa(data));
    const reader = response.body.getReader({ mode: 'byob' });

    const buffer = new ArrayBuffer(512, { maxByteLength: 1024 });
    const view = new Uint8Array(buffer);

    const result = await reader.read(view);
    reader.releaseLock();

    strictEqual(result.done, false);
    ok(result.value.byteLength > 0);
    const text = new TextDecoder().decode(result.value);
    strictEqual(text, 'A'.repeat(result.value.byteLength));
  },
};
