// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Termination of a stream transferred over JS RPC must reach the stream's origin promptly, in both
// directions of transfer and for every pairing of stream implementations:
//
// - A ReadableStream whose receiver cancels (or releases) it has its origin's cancel algorithm run
//   with the receiver's reason, even while the origin's source is idle and never writes again.
//   When the receiver tees its copy, that happens once the last branch is canceled or released,
//   and not at all when a branch reads to EOF.
// - A native WritableStream (IdentityTransformStream.writable) whose receiver aborts (or releases)
//   it has its origin's sink aborted, so the paired readable's read() rejects instead of hanging.
//
// The config runs this file in four runner/peer cells covering legacy and TypeScript stream
// implementations on each side; each cell is named by its CELL binding. Peers that return streams
// keep their execution context alive with a timer until the caller's termination arrives, so the
// observations are not confounded by the context being torn down for lack of pending work.

import * as assert from 'node:assert';
import { RpcTarget, WorkerEntrypoint } from 'cloudflare:workers';

const enc = new TextEncoder();

const REMOTE_RELEASED_MESSAGE =
  'ReadableStream sent over RPC was canceled or released by the remote execution context.';
const WRITABLE_DISCONNECTED_MESSAGE =
  'WritableStream received over RPC was disconnected because the remote execution context has ended.';

// A keep-alive bound for peers that return streams. Long enough for the caller's termination to
// arrive, short enough not to hold the test open for long if something goes wrong.
const PEER_KEEP_ALIVE_MS = 5000;

// Longer than the 32 MiB limit on values sent over JS RPC, which a cancel reason is subject to.
const OVERSIZED_REASON_LENGTH = 33 * 1024 * 1024;

// The peer services load this module too; only runners (which have a PEER binding) run the tests.
function isRunner(env) {
  return env.PEER !== undefined;
}

function describe(reason) {
  if (reason !== null && typeof reason === 'object') {
    return { name: reason.name, message: reason.message };
  }
  return { name: typeof reason, message: String(reason) };
}

// A constructor-backed readable that never produces data, reporting when it is canceled.
function makeIdleReadable() {
  const canceled = Promise.withResolvers();
  let cancelCalls = 0;
  const readable = new ReadableStream({
    cancel(reason) {
      ++cancelCalls;
      canceled.resolve(reason);
    },
  });
  return {
    readable,
    whenCanceled: canceled.promise,
    get cancelCalls() {
      return cancelCalls;
    },
  };
}

// A constructor-backed readable that delivers `text` and closes, reporting whether it is canceled.
function makeReadableWithContent(text) {
  let cancelCalls = 0;
  const readable = new ReadableStream({
    start(controller) {
      controller.enqueue(enc.encode(text));
      controller.close();
    },
    cancel() {
      ++cancelCalls;
    },
  });
  return {
    readable,
    get cancelCalls() {
      return cancelCalls;
    },
  };
}

// Handed back alongside a stream the peer returns, so the caller can observe what happened on the
// peer's side after terminating its copy.
class Probe extends RpcTarget {
  #settled;
  constructor(settled) {
    super();
    this.#settled = settled;
  }
  // Resolves with a description of the outcome once the peer observes it.
  whenSettled() {
    return this.#settled;
  }
}

export class Peer extends WorkerEntrypoint {
  async cancelReadable(stream, message) {
    assert.ok(stream instanceof ReadableStream);
    await stream.cancel(new Error(message));
  }

  // Receives a readable and lets it go without reading it.
  ignoreReadable(stream) {
    assert.ok(stream instanceof ReadableStream);
  }

  // Cancels with a reason whose serialized form exceeds the JS RPC message size limit. The reason
  // is built here because the same limit applies to RPC arguments.
  async cancelReadableWithOversizedReason(stream) {
    assert.ok(stream instanceof ReadableStream);
    await stream.cancel(new Error('x'.repeat(OVERSIZED_REASON_LENGTH)));
  }

  // Receives a readable, tees it, and cancels both branches, each with its own reason.
  async teeAndCancelBoth(stream, first, second) {
    assert.ok(stream instanceof ReadableStream);
    const [a, b] = stream.tee();
    await a.cancel(new Error(first));
    await b.cancel(new Error(second));
  }

  // Receives a readable, tees it, and lets both branches go without reading them.
  teeAndIgnoreBoth(stream) {
    assert.ok(stream instanceof ReadableStream);
    stream.tee();
  }

  // Receives a readable, tees it, cancels one branch and lets the other go without reading it.
  async teeCancelOneAndIgnoreOther(stream, message) {
    assert.ok(stream instanceof ReadableStream);
    const [a] = stream.tee();
    await a.cancel(new Error(message));
  }

  // Receives a readable, tees it, and reads both branches to EOF -- one of them through an
  // IdentityTransformStream, which a tee'd system stream's branches can be piped into.
  async teeAndReadBoth(stream) {
    assert.ok(stream instanceof ReadableStream);
    const [a, b] = stream.tee();
    return await Promise.all([
      new Response(a.pipeThrough(new IdentityTransformStream())).text(),
      new Response(b).text(),
    ]);
  }

  async abortWritable(stream, message) {
    assert.ok(stream instanceof WritableStream);
    await stream.getWriter().abort(new Error(message));
  }

  // Receives a writable and lets it go without writing to or closing it.
  ignoreWritable(stream) {
    assert.ok(stream instanceof WritableStream);
  }

  async writeAndClose(stream, text) {
    assert.ok(stream instanceof WritableStream);
    const writer = stream.getWriter();
    await writer.write(enc.encode(text));
    await writer.close();
  }

  // Returns an idle constructor-backed readable plus a probe that reports its cancel reason.
  makeIdleReadable() {
    const idle = makeIdleReadable();
    const settled = idle.whenCanceled.then((reason) => ({
      cancelCalls: idle.cancelCalls,
      reason: describe(reason),
    }));
    this.#keepAliveUntil(settled);
    return { readable: idle.readable, probe: new Probe(settled) };
  }

  // Returns both halves of a native identity stream, with a read pending on the readable half,
  // plus a probe that reports how that read settles.
  makeNativePairWithPendingRead() {
    const { readable, writable } = new IdentityTransformStream();
    const settled = readable
      .getReader()
      .read()
      .then(
        (result) => ({ status: 'fulfilled', done: result.done }),
        (reason) => ({ status: 'rejected', reason: describe(reason) })
      );
    this.#keepAliveUntil(settled);
    return { writable, probe: new Probe(settled) };
  }

  // Keeps this execution context alive -- with real pending I/O, so the runtime does not treat
  // it as hung -- until `promise` settles or the bound elapses.
  #keepAliveUntil(promise) {
    this.ctx.waitUntil(
      Promise.race([
        promise.then(
          () => {},
          () => {}
        ),
        scheduler.wait(PEER_KEEP_ALIVE_MS),
      ])
    );
  }
}

// Waits for `settled`, the origin's observation that the receiver released its copy of a stream.
// A received stream that is neither consumed nor canceled is destroyed only when the receiver's
// execution context ends. While such a stream is held, the RPC session that delivered it stays
// open, so that context ends when the runtime notices it has nothing left to do (its "hung"
// cancellation), which takes a few hundred milliseconds. The wait is kept alive with a timer so
// the runner is not itself treated as hung meanwhile, and bounded so a missing signal fails.
async function awaitReleaseObservation(settled) {
  const pending = Symbol('pending');
  for (let attempt = 0; attempt < 100; attempt++) {
    const outcome = await Promise.race([
      settled,
      scheduler.wait(50).then(() => pending),
    ]);
    if (outcome !== pending) return outcome;
  }
  throw new Error(
    'the origin never observed the receiver releasing the stream'
  );
}

// ---------------------------------------------------------------------------------------
// Readable direction: the origin's cancel algorithm runs when the receiver is done early.

export const idleReadableCanceledByReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const idle = makeIdleReadable();
    await env.PEER.cancelReadable(idle.readable, 'receiver canceled');
    const reason = await idle.whenCanceled;
    assert.strictEqual(idle.cancelCalls, 1);
    assert.ok(reason instanceof Error);
    assert.strictEqual(reason.name, 'Error');
    assert.strictEqual(reason.message, 'receiver canceled');
  },
};

export const idleReadableReleasedByReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const idle = makeIdleReadable();
    await env.PEER.ignoreReadable(idle.readable);
    // The receiver never touched its copy; the origin hears about it once that copy is destroyed.
    const reason = await awaitReleaseObservation(idle.whenCanceled);
    assert.strictEqual(idle.cancelCalls, 1);
    assert.ok(reason instanceof Error);
    assert.strictEqual(reason.message, REMOTE_RELEASED_MESSAGE);
  },
};

export const nativeReadableCanceledByReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const { readable, writable } = new IdentityTransformStream();
    await env.PEER.cancelReadable(readable, 'receiver canceled');
    // The cancel errors the identity stream at the origin, so writing into it fails. Whether the
    // first write still slips through to the (already failed) pump depends on how the cancel and
    // the write are ordered on the event loop; the second write cannot succeed either way.
    const writer = writable.getWriter();
    await assert.rejects(async () => {
      await writer.write(enc.encode('a'));
      await writer.write(enc.encode('b'));
    });
    await assert.rejects(writer.closed);
  },
};

export const returnedIdleReadableCanceledByCaller = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const { readable, probe } = await env.PEER.makeIdleReadable();
    assert.ok(readable instanceof ReadableStream);
    await readable.cancel(new Error('caller canceled'));
    const outcome = await probe.whenSettled();
    assert.strictEqual(outcome.cancelCalls, 1);
    assert.deepStrictEqual(outcome.reason, {
      name: 'Error',
      message: 'caller canceled',
    });
  },
};

export const oversizedCancelReasonIsNotSent = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const idle = makeIdleReadable();
    await env.PEER.cancelReadableWithOversizedReason(idle.readable);
    // The cancel still arrives; only the reason is left behind.
    const reason = await idle.whenCanceled;
    assert.strictEqual(idle.cancelCalls, 1);
    assert.ok(reason instanceof Error);
    assert.strictEqual(reason.message, REMOTE_RELEASED_MESSAGE);
  },
};

// ---------------------------------------------------------------------------------------
// Readable direction, tee'd by the receiver: the branches share the return channel to the origin,
// whose cancel algorithm runs once, when the last branch is done before EOF.

export const teedReadableCanceledByReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const idle = makeIdleReadable();
    await env.PEER.teeAndCancelBoth(
      idle.readable,
      'first branch',
      'second branch'
    );
    const reason = await idle.whenCanceled;
    assert.strictEqual(idle.cancelCalls, 1);
    assert.ok(reason instanceof Error);
    // The most recent reason given is the one that travels.
    assert.strictEqual(reason.message, 'second branch');
  },
};

export const teedReadableReleasedByReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const idle = makeIdleReadable();
    await env.PEER.teeAndIgnoreBoth(idle.readable);
    const reason = await awaitReleaseObservation(idle.whenCanceled);
    assert.strictEqual(idle.cancelCalls, 1);
    assert.ok(reason instanceof Error);
    assert.strictEqual(reason.message, REMOTE_RELEASED_MESSAGE);
  },
};

export const teedReadableCanceledThenReleasedByReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const idle = makeIdleReadable();
    await env.PEER.teeCancelOneAndIgnoreOther(idle.readable, 'first branch');
    // The canceled branch's reason is kept until the other branch is released along with the
    // receiver's execution context, and travels then.
    const reason = await awaitReleaseObservation(idle.whenCanceled);
    assert.strictEqual(idle.cancelCalls, 1);
    assert.ok(reason instanceof Error);
    assert.strictEqual(reason.message, 'first branch');
  },
};

export const teedReadableReadToEofByReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const content = makeReadableWithContent('delivered to both branches');
    const texts = await env.PEER.teeAndReadBoth(content.readable);
    assert.deepStrictEqual(texts, [
      'delivered to both branches',
      'delivered to both branches',
    ]);
    assert.strictEqual(content.cancelCalls, 0);
  },
};

export const returnedIdleReadableTeeCanceledBranchByBranch = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const { readable, probe } = await env.PEER.makeIdleReadable();
    const [first, second] = readable.tee();
    const settled = probe.whenSettled();
    const pending = Symbol('pending');
    const settledWithin = (ms) =>
      Promise.race([settled, scheduler.wait(ms).then(() => pending)]);
    await first.cancel(new Error('first branch'));
    // With one branch still able to read, the origin is not told yet.
    assert.strictEqual(await settledWithin(100), pending);
    await second.cancel(new Error('second branch'));
    assert.deepStrictEqual(await settledWithin(2000), {
      cancelCalls: 1,
      reason: { name: 'Error', message: 'second branch' },
    });
  },
};

// ---------------------------------------------------------------------------------------
// Writable direction: the origin's sink is aborted when the receiver is done without closing.

export const nativeWritableAbortedByReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const { readable, writable } = new IdentityTransformStream();
    const read = readable.getReader().read();
    await env.PEER.abortWritable(writable, 'receiver aborted');
    // The byte-stream protocol carries no abort reason; the origin observes a disconnection.
    await assert.rejects(read, {
      name: 'Error',
      message: WRITABLE_DISCONNECTED_MESSAGE,
    });
  },
};

export const nativeWritableReleasedByReceiver = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const { readable, writable } = new IdentityTransformStream();
    const read = readable.getReader().read();
    await env.PEER.ignoreWritable(writable);
    const settled = read.then(
      () => {
        throw new Error('read unexpectedly fulfilled');
      },
      (reason) => describe(reason)
    );
    assert.deepStrictEqual(await awaitReleaseObservation(settled), {
      name: 'Error',
      message: WRITABLE_DISCONNECTED_MESSAGE,
    });
  },
};

export const nativeWritableClosedCleanly = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const { readable, writable } = new IdentityTransformStream();
    const text = new Response(readable).text();
    await env.PEER.writeAndClose(writable, 'written by the peer');
    assert.strictEqual(await text, 'written by the peer');
  },
};

export const returnedNativeWritableAbortedByCaller = {
  async test(controller, env) {
    if (!isRunner(env)) return;
    const { writable, probe } = await env.PEER.makeNativePairWithPendingRead();
    assert.ok(writable instanceof WritableStream);
    await writable.getWriter().abort(new Error('caller aborted'));
    assert.deepStrictEqual(await probe.whenSettled(), {
      status: 'rejected',
      reason: { name: 'Error', message: WRITABLE_DISCONNECTED_MESSAGE },
    });
  },
};
