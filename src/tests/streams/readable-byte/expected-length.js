// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The workerd `expectedLength` extension on a byte source: the values the
// ReadableStream constructor accepts, and the length an accepted value
// stands for, observed as the Content-Length of a fetch body built from the
// source.
//
// Both implementations convert the value as the C++ implementation
// converts any uint64_t (ledger #35). A bigint must fit in a uint64.
// Anything else goes through ToNumber (an object's valueOf runs once), must
// be finite and non-negative, and is truncated: 1.5 is 1, '5' is 5, null is
// 0. Every failure is a TypeError. C++ lets the number 2 ** 64 through its
// range check to a cast whose result is undefined, so that one value is
// asserted for TypeScript only.
//
// DIVERGENCE (ledger #42), by design: TS also holds the source to the
// declared length as an exact total of what it delivers through enqueue(),
// respond() and respondWithNewView(), a response to a byobRequest held
// across tee() included. The delivery that goes past it throws RangeError,
// and so does a close() short of it; either errors the stream with that
// error. C++ enforces neither: every delivery reaches the reader, and
// close() closes the stream.

import { ok, rejects, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function byteSource(expectedLength, byteCount = 0) {
  return new ReadableStream({
    type: 'bytes',
    expectedLength,
    pull(controller) {
      if (byteCount > 0) controller.enqueue(new Uint8Array(byteCount));
      controller.close();
    },
  });
}

// Posts a source that declares expectedLength and delivers byteCount bytes,
// and returns the Content-Length the subrequest carried.
async function observedContentLength(env, expectedLength, byteCount) {
  const resp = await env.SELF.fetch('http://example.org', {
    method: 'POST',
    body: byteSource(expectedLength, byteCount),
  });
  await resp.arrayBuffer();
  return resp.headers.get('observed-content-length');
}

export const byteSourceExpectedLengthAccepted = {
  async test(ctrl, env) {
    // Integral lengths up to the uint64 maximum, as numbers or bigints.
    for (const value of [
      0,
      -0,
      10,
      10n,
      2 ** 53,
      2n ** 53n,
      0xffff_ffff_ffff_ffffn,
    ]) {
      ok(byteSource(value));
    }
    strictEqual(await observedContentLength(env, 10n, 10), '10');
  },
};

export const byteSourceExpectedLengthRejected = {
  test() {
    for (const value of [
      -1,
      -1n,
      -0.5,
      NaN,
      Infinity,
      -Infinity,
      'abc',
      {},
      Symbol(),
    ]) {
      throws(() => byteSource(value), TypeError);
    }
  },
};

export const byteSourceExpectedLengthCoercion = {
  async test(ctrl, env) {
    let valueOfCalls = 0;
    const object = {
      valueOf() {
        valueOfCalls++;
        return 3;
      },
    };
    for (const [value, length] of [
      [1.5, 1],
      ['5', 5],
      [null, 0],
      [true, 1],
      [object, 3],
    ]) {
      strictEqual(
        await observedContentLength(env, value, length),
        String(length)
      );
    }
    strictEqual(valueOfCalls, 1);
  },
};

export const byteSourceExpectedLengthRange = {
  test() {
    for (const value of [2n ** 64n, 2 ** 65]) {
      throws(() => byteSource(value), TypeError);
    }
    if (usingTsImpl) throws(() => byteSource(2 ** 64), TypeError);
  },
};

// A byte source declaring expectedLength, with a read(view) of 8 bytes
// pending, so that its controller holds a byobRequest.
async function pendingByobRead(expectedLength) {
  let controller;
  const rs = new ReadableStream({
    type: 'bytes',
    expectedLength,
    start(c) {
      controller = c;
    },
  });
  const reader = rs.getReader({ mode: 'byob' });
  const read = reader.read(new Uint8Array(8));
  await scheduler.wait(5);
  return { rs, controller, reader, read };
}

const OVERFLOW = {
  name: 'RangeError',
  message: 'byte source delivered more bytes than its declared expectedLength',
};

// Five bytes, one past an expectedLength of 4, through each way of
// answering the byobRequest.
const respondFive = {
  'respond()': (c) => c.byobRequest.respond(5),
  'respondWithNewView()': (c) => {
    const { buffer, byteOffset } = c.byobRequest.view;
    c.byobRequest.respondWithNewView(new Uint8Array(buffer, byteOffset, 5));
  },
};

// DIVERGENCE (ledger #42)
export const byteSourceExpectedLengthContract = {
  async test() {
    const deliverFive = {
      ...respondFive,
      'enqueue()': (c) => c.enqueue(new Uint8Array(5)),
    };
    for (const [name, deliver] of Object.entries(deliverFive)) {
      const { controller, reader, read } = await pendingByobRead(4);
      if (usingTsImpl) {
        throws(() => deliver(controller), OVERFLOW, name);
        await rejects(read, OVERFLOW);
        await rejects(reader.closed, OVERFLOW);
      } else {
        deliver(controller);
        strictEqual((await read).value.byteLength, 5, name);
        await reader.cancel('cleanup');
      }
    }

    // Exactly the declared total, then close(): parity.
    {
      const { controller, reader, read } = await pendingByobRead(4);
      controller.byobRequest.respond(4);
      controller.close();
      strictEqual((await read).value.byteLength, 4);
      await reader.closed;
    }

    // close() short of the declared total.
    {
      const { controller, reader, read } = await pendingByobRead(4);
      controller.byobRequest.respond(2);
      if (usingTsImpl) {
        const underflow =
          'byte source closed before producing its declared expectedLength';
        throws(() => controller.close(), {
          name: 'RangeError',
          message: underflow,
        });
        strictEqual((await read).value.byteLength, 2);
        await rejects(reader.closed, {
          name: 'RangeError',
          message: underflow,
        });
      } else {
        controller.close();
        strictEqual((await read).value.byteLength, 2);
        await reader.closed;
      }
    }
  },
};

// pendingByobRead's source with its reader released, the source holding
// the byobRequest over the released read, then teed: the request survives
// the fork, and a response to it reaches both branches (ledger #25).
async function teeWithHeldByobRequest(expectedLength) {
  const { rs, controller, reader, read } =
    await pendingByobRead(expectedLength);
  const request = controller.byobRequest;
  reader.releaseLock();
  await rejects(read, TypeError);
  const branches = rs.tee();
  strictEqual(controller.byobRequest, request);
  return { controller, branches };
}

// DIVERGENCE (ledger #42)
export const byteSourceExpectedLengthHeldRequest = {
  async test() {
    for (const [name, deliver] of Object.entries(respondFive)) {
      const { controller, branches } = await teeWithHeldByobRequest(4);
      if (usingTsImpl) {
        throws(() => deliver(controller), OVERFLOW, name);
        for (const branch of branches) {
          await rejects(branch.getReader().read(), OVERFLOW);
        }
      } else {
        deliver(controller);
        for (const branch of branches) {
          const { value } = await branch.getReader().read();
          strictEqual(value.byteLength, 5, name);
        }
        controller.error(new Error('cleanup'));
      }
    }

    // Exactly the declared total, then close(): parity.
    const { controller, branches } = await teeWithHeldByobRequest(4);
    controller.byobRequest.respond(4);
    controller.close();
    for (const branch of branches) {
      const reader = branch.getReader();
      strictEqual((await reader.read()).value.byteLength, 4);
      strictEqual((await reader.read()).done, true);
    }
  },
};
