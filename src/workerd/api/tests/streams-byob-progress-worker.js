// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

const sessions = new Map();
const ignore = (promise) => promise.catch(() => {});
const bytes = (value) =>
  Array.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));

async function bounded(promise) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_resolve, reject) => {
        timer = setTimeout(
          () => reject(new Error('Read did not settle after response progress')),
          1000
        );
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

async function collect(reader, count, first) {
  const result = [];
  if (first) result.push(...bytes((await first).value));
  while (result.length < count) {
    const read = await reader.read();
    if (read.done) throw new Error('Stream closed before all bytes were read');
    result.push(...bytes(read.value));
  }
  return result;
}

function start(url) {
  const state = {};
  const stream = new ReadableStream({
    type: 'bytes',
    start(controller) {
      state.controller = controller;
    },
    pull(controller) {
      if (state.finishOnPull && state.afterEnqueue) {
        state.finishOnPull = false;
        controller.enqueue(new Uint8Array([51]));
      }
    },
  });
  const [notification, rest] = stream.tee();
  const branches =
    url.searchParams.get('name') === 'threeConsumers' ? rest.tee() : [rest];
  state.reader = branches[0].getReader({ mode: 'byob' });
  state.sibling = notification.getReader();
  if (branches[1]) state.third = branches[1].getReader();
  for (const reader of [state.reader, state.sibling, state.third].filter(Boolean)) {
    ignore(reader.closed);
  }
  if (!url.searchParams.get('name').endsWith('Prefix')) {
    ignore(state.sibling.read());
  }
  sessions.set(url.search, state);
  return new Response('origin complete');
}

async function settle(url, env, state) {
  const name = url.searchParams.get('name');
  const typed = name.startsWith('typed');
  const prefix = name.endsWith('Prefix');
  const pendingReads = name === 'pendingReads';
  const closeDuringResponse = name === 'closeDuringResponse';
  const chunk = name === 'typedPrefix'
    ? [102]
    : prefix
      ? [51]
      : name === 'typedTail'
        ? [68]
        : name === 'largeEnqueue' || pendingReads
          ? [34, 51, 68]
          : [34];
  const supplied = name === 'typedPrefix'
    ? [34, 51, 68, 85]
    : typed
      ? [17, 34, 51]
      : prefix
        ? [34]
        : [17];
  const view = typed
    ? new Uint16Array(new ArrayBuffer(16), 4, 4)
    : new Uint8Array(pendingReads ? 2 : 8);
  const reads = [
    state.reader.read(view, {
      min: typed ? 1 : prefix || name === 'followupPull' ? 3 : 2,
    }),
  ];
  let request = state.controller.byobRequest;
  if (prefix) {
    request.view[0] = 17;
    request.respond(1);
    const parked = await env.SELF.fetch(`https://progress/park?${url.searchParams}`);
    if ((await parked.text()) !== 'sibling parked') {
      throw new Error('Sibling read was not parked');
    }
    request = state.controller.byobRequest;
  }
  if (typed || pendingReads) {
    reads.push(
      state.reader.read(typed ? new Uint16Array(4) : new Uint8Array(8), {
        min: typed ? 1 : 2,
      })
    );
  }
  await scheduler.wait(0);
  request.view.set(supplied);
  const retainedView = request.view;
  let entries = 0;
  let nestedError = null;
  let siblingRead;
  const descriptor = Object.getOwnPropertyDescriptor(Error.prototype, 'name');
  // Settling the parked read constructs a diagnostic synchronously, before fan-out returns.
  Object.defineProperty(Error.prototype, 'name', {
    configurable: true,
    get() {
      return 'Error';
    },
    set() {
      if (entries !== 0) return;
      entries++;
      try {
        if (closeDuringResponse) {
          state.controller.close();
          return;
        }
        if (name !== 'bufferedSibling') siblingRead = state.sibling.read();
        state.finishOnPull = name === 'followupPull';
        state.controller.enqueue(new Uint8Array(chunk));
        state.afterEnqueue = true;
      } catch (error) {
        nestedError = error.message;
      }
    },
  });
  try {
    if (url.searchParams.get('method') === 'new-view') {
      request.respondWithNewView(request.view.subarray(0, supplied.length));
    } else {
      request.respond(supplied.length);
    }
  } finally {
    Object.defineProperty(Error.prototype, 'name', descriptor);
  }
  const [origin, sibling, third] = await bounded(
    Promise.all([
      Promise.all(reads).then((results) =>
        results.map(({ value, done }) => ({
          bytes: bytes(value),
          done,
          type: value.constructor.name,
          offset: value.byteOffset,
        }))
      ),
      collect(
        state.sibling,
        closeDuringResponse ? 0 : chunk.length + (name === 'followupPull' ? 1 : 0),
        siblingRead
      ),
      state.third ? collect(state.third, 2) : [],
    ])
  );
  // Completion is checked before closing, so terminal settlement cannot hide a stalled read.
  if (!closeDuringResponse) state.controller.close();
  const closed = await bounded(
    Promise.all([
      state.reader.read(new Uint8Array(8)),
      state.sibling.read(),
      ...(state.third ? [state.third.read()] : []),
    ])
  );
  return Response.json({
    origin,
    sibling,
    third,
    entries,
    nestedError,
    wrapperView: request.view,
    retainedViewLength: retainedView.byteLength,
    closed: closed.every((read) => read.done),
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/origin') return start(url);
    const state = sessions.get(url.search);
    if (url.pathname === '/park') {
      const read = await state.sibling.read();
      if (bytes(read.value).join() !== '17') {
        throw new Error('Responded prefix was not preserved');
      }
      ignore(state.sibling.read());
      return new Response('sibling parked');
    }
    try {
      return await settle(url, env, state);
    } finally {
      sessions.delete(url.search);
    }
  },
};
