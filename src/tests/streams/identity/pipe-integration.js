// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// pipeTo()/pipeThrough() over identity streams. The implementations
// deliberately diverge (ledger #15), and both sides are asserted:
// - C++ does not implement pumping between two identity streams at all:
//   pipeTo() takes both locks and then REJECTS with TypeError
//   ("Inter-TransformStream ReadableStream.pipeTo() is not implemented."),
//   while pipeThrough() THROWS the same TypeError synchronously (this
//   particular throw is not converted by capture_async_api_throws).
// - TypeScript implements the full pipe: delivery, completion, and error
//   propagation in both directions, with the original reason instances
//   surfacing (everything stays in JavaScript).

import { ok, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { captureRejection } from 'propagation-helpers';
import { writePatternedBody, assertPatternedBytes } from 'payload-helpers';

const INTER_TS_PIPE =
  /Inter-TransformStream ReadableStream\.pipeTo\(\) is not implemented/;

async function feed(writable, text) {
  const writer = writable.getWriter();
  await writer.write(new TextEncoder().encode(text));
  await writer.close();
}

async function readAllText(readable) {
  const reader = readable.getReader();
  const dec = new TextDecoder();
  let text = '';
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    text += dec.decode(value, { stream: true });
  }
  return text + dec.decode();
}

export const pipeToBetweenIdentityStreams = {
  async test() {
    const its1 = new IdentityTransformStream();
    const its2 = new IdentityTransformStream();
    const pipePromise = its1.readable.pipeTo(its2.writable);
    // Both implementations take the locks synchronously.
    strictEqual(its1.readable.locked, true);
    strictEqual(its2.writable.locked, true);
    if (usingTsImpl) {
      const feedPromise = feed(its1.writable, 'piped');
      strictEqual(await readAllText(its2.readable), 'piped');
      await pipePromise;
      await feedPromise;
    } else {
      const err = await captureRejection(pipePromise);
      ok(err instanceof TypeError);
      ok(INTER_TS_PIPE.test(err.message));
    }
  },
};

export const pipeToDeliversLargeBody = {
  async test() {
    const its1 = new IdentityTransformStream();
    const its2 = new IdentityTransformStream();
    const pipePromise = its1.readable.pipeTo(its2.writable);
    if (usingTsImpl) {
      // Multi-megabyte body through the pipe pump, verified byte-for-byte
      // on the far side.
      const total = 2 * 1024 * 1024 + 13;
      const bodyPromise = new Response(its2.readable).arrayBuffer();
      await writePatternedBody(its1.writable, total, 65_521);
      assertPatternedBytes(new Uint8Array(await bodyPromise), total);
      await pipePromise;
    } else {
      const err = await captureRejection(pipePromise);
      ok(err instanceof TypeError);
      ok(INTER_TS_PIPE.test(err.message));
    }
  },
};

export const pipeToPropagatesSourceError = {
  async test() {
    const its1 = new IdentityTransformStream();
    const its2 = new IdentityTransformStream();
    const pipePromise = its1.readable.pipeTo(its2.writable);
    if (usingTsImpl) {
      const reason = new Error('src boom');
      await its1.writable.getWriter().abort(reason);
      // The pipe rejects with the original reason, and the destination's
      // readable side observes it too.
      strictEqual(await captureRejection(pipePromise), reason);
      strictEqual(
        await captureRejection(its2.readable.getReader().read()),
        reason
      );
    } else {
      // Error propagation is unreachable behind the not-implemented wall.
      const err = await captureRejection(pipePromise);
      ok(err instanceof TypeError);
      ok(INTER_TS_PIPE.test(err.message));
    }
  },
};

export const pipeToPropagatesDestinationError = {
  async test() {
    const its1 = new IdentityTransformStream();
    const its2 = new IdentityTransformStream();
    const pipePromise = its1.readable.pipeTo(its2.writable);
    if (usingTsImpl) {
      const reason = new Error('dst boom');
      await its2.readable.cancel(reason);
      // The pipe rejects with the original reason, and the source's
      // writable side observes it too.
      strictEqual(await captureRejection(pipePromise), reason);
      strictEqual(
        await captureRejection(
          its1.writable.getWriter().write(new Uint8Array([1]))
        ),
        reason
      );
    } else {
      const err = await captureRejection(pipePromise);
      ok(err instanceof TypeError);
      ok(INTER_TS_PIPE.test(err.message));
    }
  },
};

export const pipeThroughIdentityStream = {
  async test() {
    const its1 = new IdentityTransformStream();
    const its2 = new IdentityTransformStream();
    if (usingTsImpl) {
      const out = its1.readable.pipeThrough(its2);
      // pipeThrough returns the transform's readable side, and content
      // flows through the chain.
      strictEqual(out, its2.readable);
      const feedPromise = feed(its1.writable, 'through');
      strictEqual(await readAllText(out), 'through');
      await feedPromise;
    } else {
      // Synchronous throw, unlike pipeTo's rejected promise.
      throws(
        () => its1.readable.pipeThrough(its2),
        (err) => err instanceof TypeError && INTER_TS_PIPE.test(err.message)
      );
    }
  },
};

export const circularPipeThrough = {
  async test() {
    // Piping a stream through ITSELF: its.readable.pipeThrough(its) pumps
    // the stream into its own writable side — a deadlock generator with no
    // legitimate use.
    const its = new IdentityTransformStream();
    if (usingTsImpl) {
      // TODO(streams-ts): the circular pipe should fail, but currently
      // does not — the call succeeds, returns its own readable, and locks
      // both sides of the stream forever. When the TypeScript
      // implementation learns to reject it, flip this branch to assert
      // the failure.
      const out = its.readable.pipeThrough(its);
      strictEqual(out, its.readable);
      strictEqual(its.readable.locked, true);
      strictEqual(its.writable.locked, true);
    } else {
      // The C++ wall catches this case too, albeit for the unrelated
      // not-implemented reason.
      throws(
        () => its.readable.pipeThrough(its),
        (err) => err instanceof TypeError && INTER_TS_PIPE.test(err.message)
      );
    }
  },
};
