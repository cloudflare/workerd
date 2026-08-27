// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Abort/cancel propagation across the codec. Two divergences:
// - The canceling reader's own parked read: C++ rejects it with the cancel
//   reason; TypeScript resolves it done per WHATWG.
// - readable.cancel() reaching the writable side: TypeScript errors the
//   writable (writer.closed rejects with the reason); under C++ the
//   writable is left untouched and writer.closed stays pending.

import { strictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function macrotask() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

export const abortRejectsPendingRead = {
  async test() {
    const { writable, readable } = new CompressionStream('deflate');
    const reader = readable.getReader();
    const writer = writable.getWriter();
    const readPromise = reader.read();
    writer.abort(new Error('boom'));
    await rejects(readPromise, { message: 'boom' });
  },
};

export const cancelSettlesPendingRead = {
  async test() {
    const { readable } = new CompressionStream('deflate');
    const reader = readable.getReader();
    const readPromise = reader.read();
    reader.cancel(new Error('boom'));
    if (usingTsImpl) {
      strictEqual((await readPromise).done, true);
    } else {
      await rejects(readPromise, { message: 'boom' });
    }
  },
};

export const abortErrorsBothSides = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    await writer.write(new TextEncoder().encode('partial'));
    await writer.abort(new Error('abandon'));
    await rejects(cs.readable.getReader().read(), /abandon/);
  },
};

export const abortReasonIdentity = {
  async test() {
    // The reason reaching a pending READ diverges: C++ re-creates the
    // error across the kj boundary (same type and message under the pinned
    // enhanced_error_serialization, different instance); TypeScript
    // delivers the original instance. writer.closed receives the ORIGINAL
    // instance in both.
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    const reader = cs.readable.getReader();
    const reason = new RangeError('boom');
    const readPromise = reader.read();
    const closedExpectation = rejects(writer.closed, (err) => err === reason);
    await writer.abort(reason);
    await rejects(readPromise, (err) => {
      strictEqual(err.constructor, RangeError);
      strictEqual(err.message, 'boom');
      strictEqual(err === reason, usingTsImpl);
      return true;
    });
    await closedExpectation;
  },
};

export const writeAfterAbortDiverges = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    const reason = new RangeError('gone');
    await writer.abort(reason);
    await rejects(writer.write(new Uint8Array(1)), (err) => {
      if (usingTsImpl) {
        strictEqual(err, reason);
      } else {
        strictEqual(err.constructor, TypeError);
        strictEqual(err.message, 'This WritableStream has been closed.');
      }
      return true;
    });
  },
};

export const nonErrorAbortReasonSurfacing = {
  async test() {
    // A string abort reason reaches a pending read as an Error whose
    // message is the string under C++ (kj re-creation); TypeScript
    // delivers the original string value itself.
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    const reader = cs.readable.getReader();
    const readPromise = reader.read();
    await writer.abort('just a string');
    await rejects(readPromise, (err) => {
      if (usingTsImpl) {
        strictEqual(err, 'just a string');
      } else {
        strictEqual(err.constructor, Error);
        strictEqual(err.message, 'just a string');
      }
      return true;
    });
  },
};

export const cancelReadableWritableAftermath = {
  async test() {
    const cs = new CompressionStream('gzip');
    await cs.readable.cancel(new Error('no more'));
    const writer = cs.writable.getWriter();
    if (usingTsImpl) {
      await rejects(writer.closed, /no more/);
    } else {
      let state = 'pending';
      writer.closed.then(
        () => (state = 'resolved'),
        () => (state = 'rejected')
      );
      await macrotask();
      await macrotask();
      strictEqual(state, 'pending');
    }
  },
};
