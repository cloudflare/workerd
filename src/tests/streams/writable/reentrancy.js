// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entrancy edges: user strategy size() callbacks re-entering the
// writer mid-write, and sink hooks driving the controller. WPT
// writable-streams/reentrant-strategy.any.js passes on both
// implementations; these pins complement it with the workerd-specific
// outcomes (deferred here from the strategies suite, which pinned the
// class-strategy side).

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// A writer.write() issued from inside size() lands in the sink FIRST:
// the re-entrant write's chunk is enqueued while the outer chunk is
// still being sized (parity).
export const reentrantWriteFromSize = {
  async test() {
    const seen = [];
    let writer;
    const ws = new WritableStream(
      {
        write(v) {
          seen.push(v);
        },
      },
      {
        size(chunk) {
          if (chunk === 'outer') {
            writer.write('inner');
          }
          return 1;
        },
        highWaterMark: 10,
      }
    );
    writer = ws.getWriter();
    await writer.write('outer');
    await scheduler.wait(5);
    deepStrictEqual(seen, ['inner', 'outer']);
  },
};

// releaseLock() from inside size(): the in-progress write is charged to
// the now-released writer and rejects; the stream ends up unlocked
// (parity in behavior, per-implementation message).
export const releaseLockInsideSize = {
  async test() {
    let writer;
    const ws = new WritableStream(
      {},
      {
        size() {
          writer.releaseLock();
          return 1;
        },
        highWaterMark: 10,
      }
    );
    writer = ws.getWriter();
    await rejects(writer.write('x'), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'This writer has been released'
        : 'This WritableStream writer has been released.',
    });
    strictEqual(ws.locked, false);
  },
};

// controller.error() from inside the sink write hook: the current write
// still fulfills; the next write rejects with the hook's error (parity).
export const controllerErrorInsideWriteHook = {
  async test() {
    let controller;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      write() {
        controller.error(new Error('from-write'));
      },
    });
    const writer = ws.getWriter();
    strictEqual(await writer.write('a'), undefined);
    await rejects(writer.write('b'), { message: 'from-write' });
  },
};

// size() is NOT consulted for a doomed write: once the stream is
// errored, writer.write() rejects with the stored reason without running
// the user size callback (parity; this is the invariant the TypeScript
// sinks' willAcceptWrite fast-path relies on).
export const sizeNotCalledForDoomedWrite = {
  async test() {
    let sizeCalls = 0;
    const ws = new WritableStream(
      {},
      {
        size() {
          sizeCalls++;
          return 1;
        },
        highWaterMark: 1,
      }
    );
    const writer = ws.getWriter();
    await writer.abort('gone');
    await rejects(writer.write('x'), (e) => e === 'gone');
    strictEqual(sizeCalls, 0);
  },
};

// The user size() function is invoked with an undefined receiver and
// exactly one argument (the chunk) in both implementations.
export const sizeReceiverAndArity = {
  async test() {
    let receiver = 'unset';
    let argCount = -1;
    const strategy = {
      size(...args) {
        receiver = this === undefined ? 'undefined' : 'other';
        argCount = args.length;
        return 1;
      },
      highWaterMark: 5,
    };
    const ws = new WritableStream({}, strategy);
    const writer = ws.getWriter();
    await writer.write('x');
    strictEqual(receiver, 'undefined');
    strictEqual(argCount, 1);
  },
};

// A write made from inside sink.close() runs size() before its state
// checks reject it (WritableStreamDefaultWriterWrite step 4; the close
// algorithm clears the strategy only after it returns). Parity for the
// size() call; a throwing size() is ledger #15.
export const sizeConsultedForWriteInsideSinkClose = {
  async test() {
    for (const throws of [false, true]) {
      let sizeCalls = 0;
      let inner;
      let writer;
      const ws = new WritableStream(
        {
          close() {
            inner = writer.write('x');
          },
        },
        {
          size() {
            sizeCalls++;
            if (throws) throw new Error('size');
            return 1;
          },
          highWaterMark: 1,
        }
      );
      writer = ws.getWriter();
      const close = writer.close();
      await scheduler.wait(0);
      strictEqual(sizeCalls, 1);
      if (throws && !usingTsImpl) {
        await rejects(inner, { message: 'size' });
      } else {
        // The close in flight is checked before the erroring state, so
        // the stream that size() began erroring still rejects the write
        // as closing, and the close completes (spec; Node agrees).
        await rejects(inner, { name: 'TypeError' });
      }
      strictEqual(await close, undefined);
      strictEqual(await writer.closed, undefined);
    }
  },
};

// The same for a write made from inside sink.abort(): size() runs, then
// the write rejects with the abort reason (parity). With a throwing
// size() the write still rejects with the reason (spec; Node agrees);
// C++ rejects it with an internal error, so that shape is TS-only.
export const sizeConsultedForWriteInsideSinkAbort = {
  async test() {
    for (const throws of usingTsImpl ? [false, true] : [false]) {
      let sizeCalls = 0;
      let inner;
      let writer;
      const reason = new Error('reason');
      const ws = new WritableStream(
        {
          abort() {
            inner = writer.write('x');
          },
        },
        {
          size() {
            sizeCalls++;
            if (throws) throw new Error('size');
            return 1;
          },
          highWaterMark: 1,
        }
      );
      writer = ws.getWriter();
      await scheduler.wait(0);
      strictEqual(await writer.abort(reason), undefined);
      strictEqual(sizeCalls, 1);
      await rejects(inner, (e) => e === reason);
      await rejects(writer.closed, (e) => e === reason);
    }
  },
};
