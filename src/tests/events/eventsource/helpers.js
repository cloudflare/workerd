// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared by more than one behavior module.

// A property descriptor's attributes as a short string: its kind ('data'
// or 'accessor'), then w, e and c for writable, enumerable and
// configurable.
export function attributes(descriptor) {
  return [
    'value' in descriptor ? 'data' : 'accessor',
    descriptor.writable ? 'w' : '',
    descriptor.enumerable ? 'e' : '',
    descriptor.configurable ? 'c' : '',
  ].join('');
}

const encoder = new TextEncoder();

// A ReadableStream of `chunks`: a string is enqueued as its UTF-8 bytes,
// anything else as it is. Closed after the last chunk unless `close` is
// false.
export function streamOf(chunks, { close = true } = {}) {
  return new ReadableStream({
    start(controller) {
      for (const chunk of chunks) {
        controller.enqueue(
          typeof chunk === 'string' ? encoder.encode(chunk) : chunk
        );
      }
      if (close) controller.close();
    },
  });
}

// Resolves with the next `type` event `target` fires.
export function nextEvent(target, type) {
  return new Promise((resolve) => {
    target.addEventListener(type, resolve, { once: true });
  });
}

// Reads `chunks` to the end through EventSource.from(). Returns the
// messages, as [type, data, lastEventId], of 'message' and each of `types`,
// and the 'error' event that ends the stream.
export async function parse(chunks, types = []) {
  const source = EventSource.from(streamOf(chunks));
  const messages = [];
  for (const type of ['message', ...types]) {
    source.addEventListener(type, (event) => {
      messages.push([event.type, event.data, event.lastEventId]);
    });
  }
  const error = await nextEvent(source, 'error');
  return { messages, error };
}

// An EventSource for `path` on the suite's own server (server.js).
export function connect(env, path) {
  return new EventSource(`http://example.org${path}`, {
    fetcher: env.subrequest,
  });
}

// Records every `types` event `source` fires as [type, readyState], plus
// the error's message for 'error'.
export function record(source, types = ['open', 'message', 'error']) {
  const events = [];
  for (const type of types) {
    source.addEventListener(type, (event) => {
      events.push(
        type === 'error'
          ? [type, source.readyState, event.error?.message]
          : [type, source.readyState]
      );
    });
  }
  return events;
}
