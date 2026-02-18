// Regression test for https://github.com/cloudflare/workerd/issues/6061
// DecompressionStream generates unhandled rejections internally when
// streams are consumed via Array.fromAsync, even when the caller
// properly handles errors.

import { strictEqual } from 'node:assert';
import { mock } from 'node:test';
import { setTimeout } from 'node:timers/promises';

export const decompessionStreamUnhandledRejection = {
  async test() {
    const rejectionFn = mock.fn();
    const unhandledRejectionFn = mock.fn();
    globalThis.addEventListener('rejectionhandled', rejectionFn);
    globalThis.addEventListener('unhandledrejection', unhandledRejectionFn);

    const valid = new Uint8Array([120, 156, 75, 4, 0, 0, 98, 0, 98]); // deflate('a')
    const empty = new Uint8Array(1);
    const invalid = new Uint8Array([...valid, ...empty]);
    const double = new Uint8Array([...valid, ...valid]);
    for (const chunks of [
      [valid],
      [invalid],
      [valid, empty],
      [valid, valid],
      [double],
    ]) {
      try {
        const stream = new Blob(chunks)
          .stream()
          .pipeThrough(new DecompressionStream('deflate'));
        await Array.fromAsync(stream);
      } catch (error) {
        strictEqual(
          error.message,
          'Trailing bytes after end of compressed data'
        );
      }
    }

    await setTimeout(200);
    globalThis.removeEventListener('rejectionhandled', rejectionFn);
    globalThis.removeEventListener('unhandledrejection', unhandledRejectionFn);
    strictEqual(
      rejectionFn.mock.callCount(),
      unhandledRejectionFn.mock.callCount()
    );
  },
};
