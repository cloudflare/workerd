// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for JavaScript-backed streams (ReadableStream and WritableStream constructors)
// Ported from edgeworker streams-js.ew-test

import { strictEqual, ok, throws, rejects } from 'node:assert';

// Test that JS streams globals exist
export const userStreamsGlobalsExist = {
  test() {
    ok(ReadableStreamDefaultController !== undefined);
    ok(ReadableByteStreamController !== undefined);
    ok(ReadableStreamBYOBRequest !== undefined);
    ok(WritableStreamDefaultController !== undefined);
  },
};

// Test that JS streams objects are not directly constructable
export const jsStreamsObjectsNotConstructable = {
  test() {
    throws(() => new ReadableStreamDefaultController(), TypeError);
    throws(() => new ReadableByteStreamController(), TypeError);
    throws(() => new ReadableStreamBYOBRequest(), TypeError);
    throws(() => new WritableStreamDefaultController(), TypeError);
  },
};

// Test new ReadableStream() works
export const newReadableStream = {
  test() {
    new ReadableStream();
    new ReadableStream({ type: 'bytes' });
  },
};

// Test that underlying source algorithms are called
export const newReadableStreamAlgorithms = {
  async test() {
    // Sync algorithms
    {
      let started = false;
      let pulled = false;
      let canceled = false;
      const rs = new ReadableStream({
        start() {
          started = true;
        },
        pull() {
          pulled = true;
        },
        cancel() {
          canceled = true;
        },
      });
      ok(started);

      await scheduler.wait(1);

      rs.cancel();

      ok(pulled);
      ok(canceled);
    }

    // Byte stream sync algorithms
    {
      let started = false;
      let pulled = false;
      let canceled = false;
      const rs = new ReadableStream(
        {
          type: 'bytes',
          start() {
            started = true;
          },
          pull() {
            pulled = true;
          },
          cancel() {
            canceled = true;
          },
        },
        { highWaterMark: 1 }
      );

      ok(started);
      await scheduler.wait(1);

      rs.cancel();

      ok(pulled);
      ok(canceled);
    }

    // Async algorithms for value stream
    {
      let onStarted, onPulled, onCanceled;
      let started = new Promise((resolve) => (onStarted = resolve));
      let pulled = new Promise((resolve) => (onPulled = resolve));
      let canceled = new Promise((resolve) => (onCanceled = resolve));

      const rs = new ReadableStream({
        async start() {
          await scheduler.wait(1);
          onStarted();
        },
        async pull() {
          await scheduler.wait(1);
          onPulled();
        },
        async cancel() {
          onCanceled();
        },
      });

      await Promise.allSettled([started, pulled]);
      await scheduler.wait(1);
      await Promise.allSettled([rs.cancel(), canceled]);
    }

    // Async algorithms for byte stream
    {
      let onStarted, onPulled, onCanceled;
      let started = new Promise((resolve) => (onStarted = resolve));
      let pulled = new Promise((resolve) => (onPulled = resolve));
      let canceled = new Promise((resolve) => (onCanceled = resolve));

      const rs = new ReadableStream(
        {
          type: 'bytes',
          async start() {
            await scheduler.wait(1);
            onStarted();
          },
          async pull() {
            await scheduler.wait(1);
            onPulled();
          },
          async cancel() {
            onCanceled();
          },
        },
        { highWaterMark: 1 }
      );

      await Promise.allSettled([started, pulled]);
      await scheduler.wait(1);
      await Promise.allSettled([rs.cancel(), canceled]);
    }
  },
};

// Test that new ReadableStream creates the right kind of controller
export const newReadableStreamControllerType = {
  test() {
    new ReadableStream({
      start(c) {
        ok(c instanceof ReadableStreamDefaultController);
      },
      pull(c) {
        ok(c instanceof ReadableStreamDefaultController);
      },
    });

    new ReadableStream({
      type: 'bytes',
      start(c) {
        ok(c instanceof ReadableByteStreamController);
      },
      pull(c) {
        ok(c instanceof ReadableByteStreamController);
        const byobRequest = c.byobRequest;
        ok(byobRequest != null);
        ok(byobRequest === c.byobRequest);
        ok(byobRequest instanceof ReadableStreamBYOBRequest);
        ok(byobRequest.view instanceof Uint8Array);
      },
    });
  },
};

// Test sync algorithm errors are handled properly
export const newReadableStreamSyncAlgorithmErrorsHandled = {
  async test() {
    // Start error
    {
      const rs = new ReadableStream({
        start() {
          throw new Error('boom');
        },
      });

      await rejects(rs.getReader().read(), { message: 'boom' });
    }

    // Pull error
    {
      const rs = new ReadableStream({
        pull() {
          throw new Error('boom');
        },
      });

      await rejects(rs.getReader().read(), { message: 'boom' });
    }

    // Cancel error
    {
      const rs = new ReadableStream({
        cancel() {
          throw new Error('boom');
        },
      });
      await rejects(rs.cancel(), { message: 'boom' });
    }
  },
};

// Test async algorithm errors are handled properly
export const newReadableStreamAsyncAlgorithmErrorsHandled = {
  async test() {
    // Async start error
    {
      const rs = new ReadableStream({
        async start() {
          throw new Error('boom');
        },
      });

      await rejects(rs.getReader().read(), { message: 'boom' });
    }

    // Async pull error
    {
      const rs = new ReadableStream({
        async pull() {
          throw new Error('boom');
        },
      });

      await rejects(rs.getReader().read(), { message: 'boom' });
    }

    // Async cancel error
    {
      const rs = new ReadableStream({
        async cancel() {
          throw new Error('boom');
        },
      });

      await rejects(rs.cancel(), { message: 'boom' });
    }
  },
};
