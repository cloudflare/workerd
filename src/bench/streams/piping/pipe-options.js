// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What pipeTo()'s options cost: an AbortSignal that never fires, the
// prevent* options, and an abort that stops a pipe from an endless source
// partway.

import { CHUNK, countingWritable, enqueuedStream, expectEqual } from 'helpers';

export const pipeOptions = {
  bench(b) {
    b.run('signal that never fires x256', async () => {
      const sink = countingWritable();
      await enqueuedStream(256).pipeTo(sink.stream, {
        signal: new AbortController().signal,
      });
      expectEqual(sink.writes, 256, 'chunks written');
    });
    b.run('preventClose, preventAbort and preventCancel x256', async () => {
      const sink = countingWritable();
      await enqueuedStream(256).pipeTo(sink.stream, {
        preventClose: true,
        preventAbort: true,
        preventCancel: true,
      });
      expectEqual(sink.writes, 256, 'chunks written');
    });
    b.run('abort after 64 of an endless source', async () => {
      const controller = new AbortController();
      let writes = 0;
      const sink = new WritableStream({
        write() {
          if (++writes === 64) controller.abort();
        },
      });
      const source = new ReadableStream({
        pull(c) {
          c.enqueue(CHUNK);
        },
      });
      await source.pipeTo(sink, { signal: controller.signal }).then(
        () => {
          throw new Error('The pipe did not reject.');
        },
        () => {}
      );
      expectEqual(writes, 64, 'chunks written');
    });
  },
};
