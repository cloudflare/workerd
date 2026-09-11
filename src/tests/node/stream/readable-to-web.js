// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Readable.toWeb(): a node Readable driving a web ReadableStream. The
// adapter subscribes to 'data' and enqueues into the stream's controller,
// pausing the source whenever desiredSize drops to zero and resuming from
// pull().

import { Readable } from 'node:stream';
import { strictEqual, rejects } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

// A chunk pushed by the source arrives at the web reader.
export const toWebDeliversPushedChunk = {
  async test() {
    const r = new Readable({
      read() {
        this.push(enc.encode('ok'));
      },
    });
    const rs = Readable.toWeb(r);
    strictEqual(rs instanceof ReadableStream, true);
    const reader = rs.getReader();
    const { value, done } = await reader.read();
    strictEqual(done, false);
    strictEqual(dec.decode(value), 'ok');
  },
};

function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// Canceling the web reader destroys the node source with the cancel reason:
// the reader's cancel() resolves, and the source reports destroyed and
// errored with that reason, emitting 'error' then 'close'.
export const toWebCancelDestroysSource = {
  async test() {
    const source = new Readable({ read() {} });
    const events = [];
    source.on('error', (err) => events.push(['error', err]));
    source.on('close', () => events.push(['close']));
    const reader = Readable.toWeb(source).getReader();
    const closed = once(source, 'close');
    const reason = new Error('no longer needed');
    await reader.cancel(reason);
    await closed;
    strictEqual(source.destroyed, true);
    strictEqual(source.errored, reason);
    strictEqual(events.length, 2);
    strictEqual(events[0][0], 'error');
    strictEqual(events[0][1], reason);
    strictEqual(events[1][0], 'close');
  },
};

// Canceling without a reason still destroys the source; the node stream is
// destroyed with an AbortError, as stream.destroy() does for a stream that
// has not finished.
export const toWebCancelWithoutReasonDestroysWithAbortError = {
  async test() {
    const source = new Readable({ read() {} });
    source.on('error', () => {});
    const reader = Readable.toWeb(source).getReader();
    const closed = once(source, 'close');
    await reader.cancel();
    await closed;
    strictEqual(source.destroyed, true);
    strictEqual(source.errored?.name, 'AbortError');
    strictEqual(source.errored?.code, 'ABORT_ERR');
  },
};

// A failing pipeTo() destination cancels the adapted stream, which destroys
// the node source with the destination's error.
export const toWebPipeToFailureDestroysSource = {
  async test() {
    const source = new Readable({
      read() {
        this.push(enc.encode('x'));
      },
    });
    source.on('error', () => {});
    const boom = new Error('destination failed');
    const destination = new WritableStream({
      write() {
        throw boom;
      },
    });
    const closed = once(source, 'close');
    await rejects(Readable.toWeb(source).pipeTo(destination), (err) => {
      return err === boom;
    });
    await closed;
    strictEqual(source.destroyed, true);
    strictEqual(source.errored, boom);
  },
};
