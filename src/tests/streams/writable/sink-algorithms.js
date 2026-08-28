// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Underlying sink algorithm invocation: which hooks run, with what
// arguments and controller, and how their sync/async throws surface.
// Migrated from streams-js-test.js.
//
// DIVERGENCES pinned here (probed; see the per-test comments):
// - A synchronously-throwing start() propagates out of `new
//   WritableStream()` under TypeScript (spec) but is captured by C++,
//   which defers the error to the writer's promises. This is the
//   writable-streams/bad-underlying-sinks and start.any WPT failures.
// - abort() on an already-errored stream fulfills with undefined under
//   TypeScript (spec) but rejects with the stored error under C++ (the
//   aborting.any "multiple writer.abort()s" WPT failure family).

import { strictEqual, ok, throws, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Sync sink hooks are called with the controller; abort suppresses close.
export const newWritableStreamWithSink = {
  async test() {
    // Sync sink with abort
    {
      let started = false;
      let written = false;
      let closed = false;
      let aborted = false;
      const ws = new WritableStream({
        start(c) {
          ok(c instanceof WritableStreamDefaultController);
          started = true;
        },
        write(value, c) {
          strictEqual(value, 1);
          ok(c instanceof WritableStreamDefaultController);
          written = true;
        },
        abort(reason) {
          strictEqual(reason.message, 'boom');
          aborted = true;
        },
        close() {
          closed = true;
        },
      });

      ok(started);

      const writer = ws.getWriter();

      await writer.write(1);
      ok(written);

      await writer.abort(new Error('boom'));
      ok(aborted);
      ok(!closed);

      await rejects(writer.closed);
    }

    // Sync sink with close
    {
      let started = false;
      let written = false;
      let closed = false;
      let aborted = false;
      const ws = new WritableStream({
        start(c) {
          ok(c instanceof WritableStreamDefaultController);
          started = true;
        },
        write(value, c) {
          strictEqual(value, 1);
          ok(c instanceof WritableStreamDefaultController);
          written = true;
        },
        abort() {
          aborted = true;
        },
        close() {
          closed = true;
        },
      });

      ok(started);

      const writer = ws.getWriter();

      await writer.write(1);
      ok(written);

      await Promise.all([writer.close(), writer.closed]);

      ok(!aborted);
      ok(closed);
    }
  },
};

// Async sink hooks are awaited before the writer promises settle.
export const newWritableStreamWithSinkAsync = {
  async test() {
    // Async sink with abort
    {
      let started = false;
      let written = false;
      let closed = false;
      let aborted = false;
      const ws = new WritableStream({
        async start(c) {
          ok(c instanceof WritableStreamDefaultController);
          await scheduler.wait(10);
          started = true;
        },
        async write(value, c) {
          await scheduler.wait(10);
          strictEqual(value, 1);
          ok(c instanceof WritableStreamDefaultController);
          written = true;
        },
        async abort(reason) {
          await scheduler.wait(10);
          strictEqual(reason.message, 'boom');
          aborted = true;
        },
        async close() {
          closed = true;
        },
      });

      await scheduler.wait(15);
      ok(started);

      const writer = ws.getWriter();
      await writer.ready;

      await writer.write(1);
      ok(written);

      await writer.abort(new Error('boom'));
      ok(aborted);
      ok(!closed);

      await rejects(writer.closed);
    }

    // Async sink with close
    {
      let started = false;
      let written = false;
      let closed = false;
      let aborted = false;
      const ws = new WritableStream({
        async start(c) {
          ok(c instanceof WritableStreamDefaultController);
          await scheduler.wait(10);
          started = true;
        },
        async write(value, c) {
          await scheduler.wait(10);
          strictEqual(value, 1);
          ok(c instanceof WritableStreamDefaultController);
          written = true;
        },
        async abort() {
          await scheduler.wait(10);
          aborted = true;
        },
        async close() {
          await scheduler.wait(10);
          closed = true;
        },
      });

      await scheduler.wait(15);
      ok(started);

      const writer = ws.getWriter();
      await writer.ready;

      await writer.write(1);
      ok(written);

      await Promise.all([writer.close(), writer.closed]);
      ok(!aborted);
      ok(closed);
    }
  },
};

// start() errors (sync throw, async rejection, controller.error) surface
// on the writer's promises — except that a SYNC start throw escapes the
// constructor itself under TypeScript (spec: the constructor invokes
// start and rethrows), while C++ captures it and errors the stream.
export const newWritableStreamStartError = {
  async test() {
    // Sync start error
    if (usingTsImpl) {
      throws(
        () =>
          new WritableStream({
            start() {
              throw new Error('boom');
            },
          }),
        { message: 'boom' }
      );
    } else {
      const ws = new WritableStream({
        start() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.write(1), { message: 'boom' });
    }

    // Async start error
    {
      const ws = new WritableStream({
        async start() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.write(1), { message: 'boom' });
    }

    // Start with controller.error
    {
      const ws = new WritableStream({
        start(c) {
          c.error(new Error('boom'));
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.write(1), { message: 'boom' });
    }
  },
};

// write() errors surface on the write promise (or closed, for
// controller.error after a successful write).
export const newWritableStreamWriteError = {
  async test() {
    // Sync write error
    {
      const ws = new WritableStream({
        write() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.write(1), { message: 'boom' });
    }

    // Async write error
    {
      const ws = new WritableStream({
        async write() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.write(1), { message: 'boom' });
    }

    // Write with controller.error
    {
      const ws = new WritableStream({
        write(value, c) {
          strictEqual(value, 1);
          c.error(new Error('boom'));
        },
      });

      const writer = ws.getWriter();

      // Should succeed
      await writer.write(1);

      await rejects(writer.closed, { message: 'boom' });
    }
  },
};

// abort() hook errors reject the abort promise; controller.error inside
// the hook is ignored in favor of the abort reason.
export const newWritableStreamAbortError = {
  async test() {
    // Sync abort error
    {
      const ws = new WritableStream({
        abort(reason) {
          strictEqual(reason, 1);
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();

      await rejects(writer.abort(1), { message: 'boom' });

      // A second abort() diverges: the stream is by now errored, and the
      // spec (TypeScript) resolves abort() on an errored stream with
      // undefined, while C++ rejects again with the stored error.
      if (usingTsImpl) {
        strictEqual(await writer.abort(1), undefined);
      } else {
        await rejects(writer.abort(1), { message: 'boom' });
      }
    }

    // Async abort error
    {
      const ws = new WritableStream({
        async abort(reason) {
          strictEqual(reason, 1);
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.abort(1), { message: 'boom' });
    }

    // Abort with controller.error
    {
      let controller;
      const ws = new WritableStream({
        start(c) {
          controller = c;
        },
        async abort(reason) {
          strictEqual(reason, 1);
          controller.error(new Error('ignored'));
        },
      });

      const writer = ws.getWriter();

      await writer.abort(1);

      // The closed promise will use the abort reason, not the error
      // reported in the controller
      const results = await Promise.allSettled([writer.closed]);
      strictEqual(results[0].status, 'rejected');
      strictEqual(results[0].reason, 1);
    }
  },
};

// close() hook errors reject the close promise; controller.error inside
// the hook is ignored.
export const newWritableStreamCloseError = {
  async test() {
    // Sync close error
    {
      const ws = new WritableStream({
        close() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.close(), { message: 'boom' });
    }

    // Async close error
    {
      const ws = new WritableStream({
        async close() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.close(), { message: 'boom' });
    }

    // Close with controller.error (ignored)
    {
      let controller;
      const ws = new WritableStream({
        start(c) {
          controller = c;
        },
        async close() {
          controller.error(new Error('ignored'));
        },
      });

      const writer = ws.getWriter();

      // In this case, the error reported in the close algorithm is ignored
      await Promise.all([writer.close(), writer.closed]);
    }
  },
};

// The user strategy size() is consulted per write; its throws doom the
// write.
export const writableStreamSizeAlgorithm = {
  async test() {
    // Size algorithm called
    {
      let sizeCalled = false;
      const ws = new WritableStream(
        {
          async write() {
            await scheduler.wait(10);
          },
        },
        {
          highWaterMark: 2,
          size(value) {
            sizeCalled = true;
            strictEqual(value, 'hello');
            return 2;
          },
        }
      );

      const writer = ws.getWriter();
      strictEqual(writer.desiredSize, 2);
      const write = writer.write('hello');
      ok(sizeCalled);
      strictEqual(writer.desiredSize, 0);
      await write;
    }

    // Size algorithm error
    {
      const ws = new WritableStream(
        {},
        {
          size() {
            throw new Error('boom');
          },
        }
      );

      const writer = ws.getWriter();
      await rejects(writer.write('hello'), { message: 'boom' });
    }
  },
};

// The sink dictionary is converted once at construction: hook property
// getters are read exactly once regardless of how many times the hooks
// later run (parity).
export const sinkHooksCapturedAtConstruction = {
  async test() {
    let writeGets = 0;
    let closeGets = 0;
    const sink = {};
    Object.defineProperty(sink, 'write', {
      get() {
        writeGets++;
        return () => {};
      },
    });
    Object.defineProperty(sink, 'close', {
      get() {
        closeGets++;
        return () => {};
      },
    });
    const ws = new WritableStream(sink);
    const writer = ws.getWriter();
    await writer.write('a');
    await writer.write('b');
    await writer.close();
    strictEqual(writeGets, 1);
    strictEqual(closeGets, 1);
  },
};

// A sink write() returning a rejected promise on a later write errors the
// stream: that write, a replaced ready, and closed all reject with the
// sink's error (parity; the WPT bad-underlying-sinks "second write"
// case).
export const secondWriteRejectionErrorsStream = {
  async test() {
    let count = 0;
    const ws = new WritableStream({
      write() {
        count++;
        if (count === 2) return Promise.reject(new Error('bad2'));
        return Promise.resolve();
      },
    });
    const writer = ws.getWriter();
    await writer.write('one');
    const readyBefore = writer.ready;
    await rejects(writer.write('two'), { message: 'bad2' });
    ok(readyBefore !== writer.ready, 'ready must be replaced');
    await rejects(writer.ready, { message: 'bad2' });
    await rejects(writer.closed, { message: 'bad2' });
  },
};

// After start() throws, the sink's write/close hooks are never invoked in
// either implementation, no matter how the failure surfaced (constructor
// throw vs deferred rejection; the WPT start.any case).
export const sinkHooksNotCalledAfterStartThrow = {
  async test() {
    let writeCalled = false;
    let closeCalled = false;
    let ws;
    try {
      ws = new WritableStream({
        start() {
          throw new Error('boom');
        },
        write() {
          writeCalled = true;
        },
        close() {
          closeCalled = true;
        },
      });
      ok(!usingTsImpl, 'only the C++ constructor captures the throw');
    } catch {
      ok(usingTsImpl, 'only the TypeScript constructor rethrows');
    }
    if (ws) {
      const writer = ws.getWriter();
      await writer.write('x').catch(() => {});
      await writer.close().catch(() => {});
      await writer.closed.catch(() => {});
    }
    strictEqual(writeCalled, false);
    strictEqual(closeCalled, false);
  },
};
