// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A reader's closed promise is marked handled where it is made, so one that
// rejects while nothing looks at it reports no unhandledrejection (parity).
// No case looks at a reader's closed promise: each reaches one of the places
// a reader makes it, then rejects it.

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
  // made pending on a readable stream, rejected when it errors.
  erroredAfterAcquisition() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    rs.getReader();
    controller.error(boom());
  },
  // made pending, rejected at release.
  releasedFromReadable() {
    new ReadableStream().getReader().releaseLock();
  },
  // made rejected on an errored stream.
  acquiredOnErrored() {
    const rs = new ReadableStream({
      start(c) {
        c.error(boom());
      },
    });
    rs.getReader();
  },
  // made settled on a closed stream, replaced by a rejected one at release.
  releasedFromClosed() {
    const rs = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    rs.getReader().releaseLock();
  },
};

export const readerClosedIsMarkedHandled = {
  async test() {
    deepStrictEqual(await casesReportingUnhandled(cases), []);
  },
};
