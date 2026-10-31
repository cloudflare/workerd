// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The reason a receiver cancels a ReadableStream transferred over JS RPC with travels back to the
// stream's origin only when the receiver serialized it with enhanced error serialization, which
// lets the origin read it without the stacks of the errors it contains. Without that flag, V8's
// native error encoding would hand the origin the receiver's stack, so the origin gets the generic
// release error instead, whatever the reason was and whatever the origin's own flag is.
//
// The config runs this file in four runner/peer cells covering each pairing of the two error
// serialization modes. Each worker's module is named after its mode, so a test can spot the other
// side's frames in a stack. Tests cover both directions: the runner as origin (the peer receives
// and cancels) and the runner as receiver (the peer returns a stream for the runner to cancel).

import * as assert from 'node:assert';
import { RpcTarget, WorkerEntrypoint } from 'cloudflare:workers';

const REMOTE_RELEASED_MESSAGE =
  'ReadableStream sent over RPC was canceled or released by the remote execution context.';

// A keep-alive bound for peers that return streams. Long enough for the caller's cancel to
// arrive, short enough not to hold the test open for long if something goes wrong.
const PEER_KEEP_ALIVE_MS = 5000;

// The peer services load this module too; only runners (which have a PEER binding) run the tests.
function isRunner(env) {
  return env.PEER !== undefined;
}

// A constructor-backed readable that never produces data, reporting when it is canceled.
function makeIdleReadable() {
  const canceled = Promise.withResolvers();
  const readable = new ReadableStream({
    cancel(reason) {
      canceled.resolve(reason);
    },
  });
  return { readable, whenCanceled: canceled.promise };
}

function describeError(error) {
  return {
    isError: error instanceof Error,
    message: error?.message,
    stack: String(error?.stack ?? ''),
  };
}

// Checks the error the origin's cancel algorithm received, as described by describeError(). It is
// the receiver's error when the receiver used enhanced error serialization, and the generic release
// error otherwise. Either way it carries none of the receiver's frames.
function assertErrorReason(
  described,
  { receiverEnhanced, receiverModule, message }
) {
  assert.ok(
    !described.stack.includes(receiverModule),
    `the origin received the receiver's stack frames:\n${described.stack}`
  );
  assert.ok(described.isError);
  assert.strictEqual(
    described.message,
    receiverEnhanced ? message : REMOTE_RELEASED_MESSAGE
  );
}

class Probe extends RpcTarget {
  #settled;
  constructor(settled) {
    super();
    this.#settled = settled;
  }
  whenSettled() {
    return this.#settled;
  }
}

export class Peer extends WorkerEntrypoint {
  async cancelWithError(stream, message) {
    assert.ok(stream instanceof ReadableStream);
    await stream.cancel(new Error(message));
  }

  async cancelWithNestedError(stream, message) {
    assert.ok(stream instanceof ReadableStream);
    await stream.cancel({ inner: new Error(message) });
  }

  async cancelWithString(stream, reason) {
    assert.ok(stream instanceof ReadableStream);
    await stream.cancel(reason);
  }

  // Returns an idle readable plus a probe that reports the error its cancel algorithm received.
  makeIdleReadable() {
    const idle = makeIdleReadable();
    const settled = idle.whenCanceled.then(describeError);
    this.ctx.waitUntil(
      Promise.race([settled, scheduler.wait(PEER_KEEP_ALIVE_MS)])
    );
    return { readable: idle.readable, probe: new Probe(settled) };
  }
}

// ---------------------------------------------------------------------------------------
// The runner is the origin; the peer receives the stream and cancels it.

export const errorReasonFromReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const idle = makeIdleReadable();
    await env.PEER.cancelWithError(idle.readable, 'receiver canceled');
    assertErrorReason(describeError(await idle.whenCanceled), {
      receiverEnhanced: env.PEER_ENHANCED,
      receiverModule: env.PEER_MODULE,
      message: 'receiver canceled',
    });
  },
};

export const nestedErrorReasonFromReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const idle = makeIdleReadable();
    await env.PEER.cancelWithNestedError(idle.readable, 'nested');
    // The reason arrives as sent, or as the generic release error in its place.
    const reason = await idle.whenCanceled;
    assertErrorReason(
      describeError(reason instanceof Error ? reason : reason.inner),
      {
        receiverEnhanced: env.PEER_ENHANCED,
        receiverModule: env.PEER_MODULE,
        message: 'nested',
      }
    );
  },
};

// A reason that carries no stack is still left behind when the receiver lacks enhanced error
// serialization: the receiver cannot tell what the serialized reason contains.
export const stringReasonFromReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const idle = makeIdleReadable();
    await env.PEER.cancelWithString(idle.readable, 'receiver canceled');
    const reason = await idle.whenCanceled;
    if (env.PEER_ENHANCED) {
      assert.strictEqual(reason, 'receiver canceled');
    } else {
      assert.ok(reason instanceof Error);
      assert.strictEqual(reason.message, REMOTE_RELEASED_MESSAGE);
    }
  },
};

// ---------------------------------------------------------------------------------------
// The peer is the origin; the runner receives the stream and cancels it.

export const errorReasonFromCaller = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const { readable, probe } = await env.PEER.makeIdleReadable();
    assert.ok(readable instanceof ReadableStream);
    await readable.cancel(new Error('caller canceled'));
    assertErrorReason(await probe.whenSettled(), {
      receiverEnhanced: env.SELF_ENHANCED,
      receiverModule: env.SELF_MODULE,
      message: 'caller canceled',
    });
  },
};
