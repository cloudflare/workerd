// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A writer's ready and closed promises are marked handled where they are
// made, so one that rejects while nothing looks at it reports no
// unhandledrejection (parity). No case looks at a writer's ready or closed
// promise: each reaches one of the places a writer makes them, then rejects
// it.

import { deepStrictEqual } from 'node:assert';

const boom = () => new Error('boom');

// Runs each case under an unhandledrejection listener, waiting a beat after
// it for late reports, and names the cases that reported one.
async function casesReportingUnhandled(cases) {
  const reported = [];
  let current;
  const listener = (event) => {
    reported.push(current);
    event.preventDefault();
  };
  globalThis.addEventListener('unhandledrejection', listener);
  try {
    for (const [name, run] of Object.entries(cases)) {
      current = name;
      await run();
      await scheduler.wait(20);
    }
  } finally {
    globalThis.removeEventListener('unhandledrejection', listener);
  }
  return reported;
}

const cases = {
  // closed made pending on a writable stream; ready made settled, then
  // replaced by a rejected one.
  async erroredAfterAcquisition() {
    let controller;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
    });
    ws.getWriter();
    await scheduler.wait(1);
    controller.error(boom());
  },
  // ready made pending under backpressure.
  async erroredUnderBackpressure() {
    let controller;
    const ws = new WritableStream(
      {
        start(c) {
          controller = c;
        },
      },
      { highWaterMark: 0 }
    );
    ws.getWriter();
    await scheduler.wait(1);
    controller.error(boom());
  },
  // ready replaced by a pending one at capacity.
  async erroredAtCapacity() {
    let controller;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      write() {
        return new Promise(() => {});
      },
    });
    const writer = ws.getWriter();
    await scheduler.wait(1);
    writer.write('a').catch(() => {});
    controller.error(boom());
  },
  // ready made rejected and closed made pending while the stream errors.
  async acquiredWhileErroring() {
    let controller, finishWrite;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      write() {
        return new Promise((resolve) => {
          finishWrite = resolve;
        });
      },
    });
    const first = ws.getWriter();
    first.write('a').catch(() => {});
    await scheduler.wait(1);
    controller.error(boom());
    first.releaseLock();
    ws.getWriter();
    finishWrite();
  },
  // both made rejected on an errored stream.
  async acquiredOnErrored() {
    const ws = new WritableStream({
      start(c) {
        c.error(boom());
      },
    });
    await scheduler.wait(1);
    ws.getWriter();
  },
  // both settled, then replaced by rejected ones at release.
  async releasedAfterClose() {
    const ws = new WritableStream();
    const writer = ws.getWriter();
    await writer.close();
    writer.releaseLock();
  },
};

export const writerPromisesAreMarkedHandled = {
  async test() {
    deepStrictEqual(await casesReportingUnhandled(cases), []);
  },
};
