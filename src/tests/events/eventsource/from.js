// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// EventSource.from(), the Workers extension that reads an event stream from
// a ReadableStream: its argument checks, the open it fires synchronously,
// and how it reads each kind of chunk.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { nextEvent, parse, streamOf } from 'helpers';
import { usingTsStreams } from 'which-impl';

export const fromRejectsNonStreams = {
  test() {
    for (const value of [undefined, null, {}, 'x', new Response('x')]) {
      throws(() => EventSource.from(value), {
        name: 'TypeError',
        message:
          "Failed to execute 'from' on 'EventSource': parameter 1 is not of " +
          "type 'JsReadableStream'.",
      });
    }
  },
};

export const fromRejectsLockedAndDisturbedStreams = {
  async test() {
    const locked = streamOf([]);
    locked.getReader();
    throws(() => EventSource.from(locked), {
      name: 'TypeError',
      message: 'This ReadableStream is locked.',
    });

    const disturbed = streamOf(['x']);
    const reader = disturbed.getReader();
    await reader.read();
    reader.releaseLock();
    throws(() => EventSource.from(disturbed), {
      name: 'TypeError',
      message: 'This ReadableStream has already been read from.',
    });
  },
};

export const fromNeedsNoReceiver = {
  test() {
    const { from } = EventSource;
    const source = from(streamOf([], { close: false }));
    strictEqual(Object.getPrototypeOf(source), EventSource.prototype);
    source.close();
  },
};

export const fromOpensSynchronously = {
  test() {
    const stream = streamOf([], { close: false });
    const source = EventSource.from(stream);
    strictEqual(source.readyState, EventSource.OPEN);
    strictEqual(source.url, '');
    strictEqual(source.withCredentials, false);
    ok(stream.locked);
    source.close();
  },
};

// The end of the stream closes an EventSource.from() source, with no
// reconnection.
export const fromReadsToTheEnd = {
  async test() {
    const chunks = ['data: first\n\n', 'data: second\n\n', 'data: third\n\n'];
    const encoder = new TextEncoder();
    const source = EventSource.from(
      new ReadableStream({
        async pull(controller) {
          await scheduler.wait(10);
          controller.enqueue(encoder.encode(chunks.shift()));
          if (chunks.length === 0) controller.close();
        },
      })
    );
    const data = [];
    source.onmessage = (event) => data.push(event.data);
    const error = await nextEvent(source, 'error');
    deepStrictEqual(data, ['first', 'second', 'third']);
    strictEqual(error.error.message, 'The server disconnected.');
    strictEqual(source.readyState, EventSource.CLOSED);
  },
};

// A buffer or any view is read as its bytes. Anything else fails the
// EventSource with the TypeError of the streams implementation's own byte
// reads.
export const fromChunkKinds = {
  async test() {
    const bytes = (text) => new TextEncoder().encode(text);
    const { messages } = await parse([
      bytes('data: caf\u00e9\n\n'),
      bytes('data: array buffer\n\n').buffer,
      new DataView(bytes('data: data view\n\n').buffer),
      new Uint16Array(bytes('data: u16!\n\n').buffer),
      bytes('xxdata: offset\n\n').subarray(2),
    ]);
    deepStrictEqual(
      messages.map(([, data]) => data),
      ['caf\u00e9', 'array buffer', 'data view', 'u16!', 'offset']
    );
  },
};

// A string chunk, enqueued as it is, is read as its UTF-8 bytes.
export const fromStringChunks = {
  async test() {
    const source = EventSource.from(
      new ReadableStream({
        start(controller) {
          controller.enqueue('data: caf\u00e9\n\n');
          controller.enqueue(new TextEncoder().encode('data: bytes\n\n'));
          controller.close();
        },
      })
    );
    const data = [];
    source.onmessage = (event) => data.push(event.data);
    await nextEvent(source, 'error');
    deepStrictEqual(data, ['caf\u00e9', 'bytes']);
  },
};

export const fromRejectsOtherChunks = {
  async test() {
    for (const chunk of [5, {}]) {
      const source = EventSource.from(streamOf([chunk]));
      const event = await nextEvent(source, 'error');
      strictEqual(event.error.name, 'TypeError');
      strictEqual(
        event.error.message,
        usingTsStreams
          ? 'This ReadableStream did not return bytes.'
          : 'Draining read encountered a value that cannot be converted to bytes'
      );
      strictEqual(source.readyState, EventSource.CLOSED);
    }
  },
};
