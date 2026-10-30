// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// close() and disposal. Divergences (see AGENTS.md):
// - #10: once a listener closes the TypeScript EventSource, the rest of the
//   messages read with that one are dropped; C++ dispatches them.
// - #11: closing an EventSource.from() source cancels its stream in the
//   TypeScript implementation; C++ goes on reading it.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { connect, nextEvent, record, streamOf } from 'helpers';
import { usingTsImpl } from 'which-impl';

export const closeIsIdempotentAndSilent = {
  async test(ctrl, env) {
    const source = connect(env, '/request-headers');
    const events = record(source);
    source.close();
    source.close();
    strictEqual(source.readyState, EventSource.CLOSED);
    await scheduler.wait(50);
    deepStrictEqual(events, []);
  },
};

export const disposeCloses = {
  test() {
    const source = EventSource.from(streamOf([], { close: false }));
    strictEqual(source.readyState, EventSource.OPEN);
    source[Symbol.dispose]();
    strictEqual(source.readyState, EventSource.CLOSED);
  },
};

// #10.
export const closeFromAListener = {
  async test() {
    const source = EventSource.from(streamOf(['data: a\n\ndata: b\n\n']));
    const data = [];
    source.onmessage = (event) => {
      data.push(event.data);
      source.close();
    };
    await scheduler.wait(20);
    deepStrictEqual(data, usingTsImpl ? ['a'] : ['a', 'b']);
  },
};

// #11.
export const closeCancelsAFromStream = {
  async test() {
    let pulls = 0;
    let cancelled = false;
    const encoder = new TextEncoder();
    const source = EventSource.from(
      new ReadableStream(
        {
          async pull(controller) {
            await scheduler.wait(1);
            pulls++;
            controller.enqueue(encoder.encode('data: x\n\n'));
          },
          cancel() {
            cancelled = true;
          },
        },
        { highWaterMark: 0 }
      )
    );
    await nextEvent(source, 'message');
    source.close();
    // Let a pull already under way finish.
    await scheduler.wait(10);
    const before = pulls;
    await scheduler.wait(30);
    deepStrictEqual(
      [cancelled, pulls > before],
      usingTsImpl ? [true, false] : [false, true]
    );
  },
};
