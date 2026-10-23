// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

const sessions = new Map();
const ignore = (promise) => promise.catch(() => {});

async function cachedResponse(url) {
  if (url.pathname === '/origin') {
    const session = {};
    const stream = new ReadableStream({
      type: 'bytes',
      start(controller) {
        session.controller = controller;
      },
    });
    const [notification, byob] = stream.tee();
    session.streams = [notification, byob];
    session.notification = notification.getReader();
    session.reader = byob.getReader({ mode: 'byob' });
    ignore(session.notification.closed);
    ignore(session.reader.closed);
    ignore(session.notification.read());
    sessions.set(url.search, session);
    return new Response('origin complete');
  }

  const { controller, notification, reader, streams } = sessions.get(
    url.search
  );
  sessions.delete(url.search);
  const pending = reader.read(new Uint8Array(8));
  const cached = controller.byobRequest;
  const viewBefore = cached.view.byteLength;
  let entries = 0;
  let validationError = null;
  let responded = false;
  let viewCleared = false;
  let cachedCleared = false;
  let desiredSize;
  const descriptor = Object.getOwnPropertyDescriptor(Error.prototype, 'name');
  Object.defineProperty(Error.prototype, 'name', {
    configurable: true,
    get() {
      return 'Error';
    },
    set() {
      if (entries !== 0) return;
      entries++;
      desiredSize = controller.desiredSize;
      if (url.searchParams.get('method') === 'new-view') {
        if (url.searchParams.get('pedantic') === 'true') {
          try {
            cached.respondWithNewView(cached.view.subarray(1, 1));
          } catch (error) {
            validationError = error.name;
          }
        }
        cached.respondWithNewView(cached.view.subarray(0, 0));
      } else {
        cached.respond(0);
      }
      responded = true;
      viewCleared = cached.view === null;
      cachedCleared = controller.byobRequest === null;
    },
  });
  try {
    controller.close();
  } finally {
    Object.defineProperty(Error.prototype, 'name', descriptor);
  }
  const result = await pending;
  const subsequent = await reader.read(new Uint8Array(8));
  // The original closed promises belong to the expired request. Reacquire readers only
  // after the current reads settle, so fresh promises observe the stream's terminal state.
  notification.releaseLock();
  reader.releaseLock();
  const freshReaders = streams.map((stream, index) =>
    stream.getReader(index === 1 ? { mode: 'byob' } : undefined)
  );
  const fresh = await Promise.all([
    freshReaders[0].read().then(({ done }) => ({ done })),
    freshReaders[1].read(new Uint8Array(8)).then(({ done }) => ({ done })),
  ]);
  const closed = await Promise.all(
    freshReaders.map((reader) => reader.closed.then(() => 'closed'))
  );
  return Response.json({
    entries,
    desiredSize,
    viewBefore,
    validationError,
    responded,
    viewCleared,
    cachedCleared,
    current: { done: result.done, length: result.value.byteLength },
    subsequent: { done: subsequent.done, length: subsequent.value.byteLength },
    fresh,
    closed,
  });
}

export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.searchParams.has('cached')) return cachedResponse(url);
    const kind = url.searchParams.get('kind');
    const operation = url.searchParams.get('operation');
    const buffered = url.searchParams.has('buffered');
    const key = url.search;
    const options = kind === 'byob' ? { mode: 'byob' } : undefined;
    const read = (reader) =>
      kind === 'byob' ? reader.read(new Uint8Array(8)) : reader.read();
    if (url.pathname === '/origin') {
      const session = {};
      const source = {
        start(controller) {
          session.controller = controller;
        },
      };
      if (kind !== 'values') source.type = 'bytes';
      const stream = new ReadableStream(source);
      const [first, rest] = stream.tee();
      const [second, third] = rest.tee();
      session.streams = [first, second, third];
      session.readers = session.streams.map((branch) =>
        branch.getReader(options)
      );
      for (const reader of session.readers) ignore(reader.closed);
      // These reads belong to a completed request. Their settlement emits the synchronous
      // diagnostic used below, before fan-out reaches the remaining consumers.
      const pendingReadCount = buffered ? 1 : 12;
      for (let i = 0; i < pendingReadCount; i++) {
        const pending = buffered
          ? session.readers[0].read(new Uint8Array(8), { min: 2 })
          : read(session.readers[0]);
        ignore(pending);
        if (buffered && i === 0) {
          session.prefixSettled = false;
          ignore(
            pending.then(() => {
              session.prefixSettled = true;
            })
          );
        }
      }
      if (buffered) session.controller.enqueue(new Uint8Array([17]));
      sessions.set(key, session);
      return new Response('origin complete');
    }

    const session = sessions.get(key);
    sessions.delete(key);
    const { controller, readers, streams } = session;
    if (buffered && session.prefixSettled) {
      throw new Error('Buffered read settled before the terminal operation');
    }
    const observe = (promise) =>
      promise.then(
        ({ value, done }) =>
          buffered ? { done, bytes: Array.from(value) } : { done },
        (error) => ({ error: error.message })
      );
    const current = readers.map((reader) => observe(read(reader)));
    let entries = 0;
    let desiredSize;
    let nestedError = null;
    let outerReturned = false;
    const descriptor = Object.getOwnPropertyDescriptor(Error.prototype, 'name');
    Object.defineProperty(Error.prototype, 'name', {
      configurable: true,
      get() {
        return 'Error';
      },
      set() {
        if (entries !== 0) return;
        entries++;
        desiredSize = controller.desiredSize;
        try {
          if (operation === 'close')
            controller.error(new Error('nested error'));
          else controller.close();
        } catch (error) {
          nestedError = error.name;
        }
      },
    });
    try {
      if (operation === 'close') controller.close();
      else controller.error(new Error('source error'));
      outerReturned = true;
    } finally {
      Object.defineProperty(Error.prototype, 'name', descriptor);
    }
    const subsequent = readers.map((reader) => observe(read(reader)));
    const currentResults = await Promise.all(current);
    const subsequentResults = await Promise.all(subsequent);
    for (const reader of readers) reader.releaseLock();
    const freshReaders = streams.map((stream) => stream.getReader(options));
    const fresh = freshReaders.map((reader) => observe(read(reader)));
    const closed = freshReaders.map((reader) =>
      reader.closed.then(
        () => 'closed',
        (error) => error.message
      )
    );
    return Response.json({
      entries,
      desiredSize,
      nestedError,
      outerReturned,
      current: currentResults,
      subsequent: subsequentResults,
      fresh: await Promise.all(fresh),
      closed: await Promise.all(closed),
      ...(buffered ? { prefixPending: !session.prefixSettled } : {}),
    });
  },
};
