// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// How readable.cancel() settles when the writable side is aborted,
// terminated, errored or closed in the same turn, on a started stream,
// with and without transformer hooks (the latter take the zero-hook
// path). The spec reads the writable's state when the cancel algorithm
// settles (TransformStreamDefaultSourceCancelAlgorithm step 7.1.1): if it
// has errored by then, the cancel rejects with its stored error.
// TypeScript follows the spec (Node agrees on every case); C++ diverges
// (ledger #16), and only the parts stable across its cells are asserted.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const outcome = (p) =>
  p.then(
    () => 'fulfilled',
    (e) => e
  );

const kHooks = ['none', 'cancel'];

async function startedTransform(hooks, writableStrategy) {
  let controller;
  const ts = new TransformStream(
    hooks === 'none'
      ? undefined
      : {
          start(c) {
            controller = c;
          },
          cancel() {},
        },
    writableStrategy
  );
  await scheduler.wait(1);
  return { ts, controller };
}

// cancel(r1) then abort(r2): the abort errors the writable before the
// cancel settles, so the cancel, the abort and closed reject with r2.
export const cancelThenAbortSameTurn = {
  async test() {
    const r1 = new Error('r1');
    const r2 = new Error('r2');
    for (const hooks of kHooks) {
      const { ts } = await startedTransform(hooks);
      const writer = ts.writable.getWriter();
      const cancel = outcome(ts.readable.cancel(r1));
      const abort = outcome(writer.abort(r2));
      const closed = outcome(writer.closed);
      if (usingTsImpl) {
        strictEqual(await cancel, r2);
        strictEqual(await abort, r2);
        strictEqual(await closed, r2);
      } else {
        strictEqual(await cancel, 'fulfilled');
        strictEqual(await abort, 'fulfilled');
        if (hooks === 'none') strictEqual(await closed, r1);
      }
    }
  },
};

// abort(r2) then cancel(r1): the cancel returns the abort's finish
// promise, which fulfills; closed rejects with r2.
export const abortThenCancelSameTurn = {
  async test() {
    const r1 = new Error('r1');
    const r2 = new Error('r2');
    for (const hooks of kHooks) {
      const { ts } = await startedTransform(hooks);
      const writer = ts.writable.getWriter();
      const abort = outcome(writer.abort(r2));
      const cancel = outcome(ts.readable.cancel(r1));
      strictEqual(await abort, 'fulfilled');
      strictEqual(
        await cancel,
        !usingTsImpl && hooks === 'none' ? r2 : 'fulfilled'
      );
      strictEqual(await outcome(writer.closed), r2);
    }
  },
};

// cancel() then terminate() once started: the terminate errors the
// writable before the cancel settles, so the cancel rejects with the
// terminate TypeError. (Before start the writable is still erroring when
// the cancel settles, so it fulfills: terminateAfterReadableCancel.)
export const cancelThenTerminateAfterStart = {
  async test() {
    const reason = new Error('cancel-reason');
    const { ts, controller } = await startedTransform('cancel');
    const writer = ts.writable.getWriter();
    const cancel = outcome(ts.readable.cancel(reason));
    if (usingTsImpl) {
      controller.terminate();
      const err = await cancel;
      strictEqual(err.name, 'TypeError');
      strictEqual(err.message, 'The transform stream has been terminated');
      strictEqual(await outcome(writer.closed), err);
    } else {
      throws(() => controller.terminate(), {
        name: 'TypeError',
        message: 'This ReadableStream is closed.',
      });
      strictEqual(await cancel, 'fulfilled');
      strictEqual(await outcome(writer.closed), reason);
    }
  },
};

// A write whose writable size() throws errors the writable; a cancel in
// the same turn rejects with that error.
export const cancelAfterWritableSizeError = {
  async test() {
    const sizeError = new Error('size');
    for (const hooks of kHooks) {
      const { ts } = await startedTransform(hooks, {
        size() {
          throw sizeError;
        },
      });
      const writer = ts.writable.getWriter();
      const write = outcome(writer.write('x'));
      const cancel = outcome(ts.readable.cancel(new Error('r1')));
      strictEqual(await cancel, usingTsImpl ? sizeError : 'fulfilled');
      strictEqual(await write, sizeError);
      strictEqual(await outcome(writer.closed), sizeError);
    }
  },
};

// cancel() then close(): the close runs the sink close, which returns the
// cancel's finish promise; everything fulfills.
export const cancelThenCloseSameTurn = {
  async test() {
    const reason = new Error('r1');
    for (const hooks of kHooks) {
      const { ts } = await startedTransform(hooks);
      const writer = ts.writable.getWriter();
      const cancel = outcome(ts.readable.cancel(reason));
      const close = outcome(writer.close());
      const closed = outcome(writer.closed);
      strictEqual(await cancel, 'fulfilled');
      if (usingTsImpl) {
        strictEqual(await close, 'fulfilled');
        strictEqual(await closed, 'fulfilled');
      } else if (hooks === 'none') {
        strictEqual((await close) instanceof Error, true);
        strictEqual(await closed, reason);
      } else {
        strictEqual(await close, 'fulfilled');
      }
    }
  },
};
