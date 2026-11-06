// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An EventSource works with the built-ins it relies on patched: every
// method or accessor of theirs it reaches is captured before user code
// runs. The C++ implementation calls none of them, but when it reads the
// TypeScript streams, the native pump resolves a promise with another, which
// looks up Promise.prototype.then, so that combination leaves it unpatched.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { connect, nextEvent, streamOf } from 'helpers';
import { usingTsImpl, usingTsStreams } from 'which-impl';

// Replaces each of `names` on `object` with a function that throws, and
// returns a function that restores them. Index loops and no array
// destructuring: both would reach a patched Array.prototype[Symbol.iterator].
function patch(object, names) {
  const saved = [];
  for (let i = 0; i < names.length; i++) {
    const name = names[i];
    const descriptor = Object.getOwnPropertyDescriptor(object, name);
    if (descriptor === undefined) continue;
    saved[saved.length] = { name, descriptor };
    const fail = () => {
      throw new Error(`patched ${String(name)} called`);
    };
    Object.defineProperty(
      object,
      name,
      'value' in descriptor
        ? { ...descriptor, value: fail }
        : { ...descriptor, get: fail, set: descriptor.set && fail }
    );
  }
  return () => {
    for (let i = 0; i < saved.length; i++) {
      Object.defineProperty(object, saved[i].name, saved[i].descriptor);
    }
  };
}

// `promises`: whether to patch Promise.prototype's methods too. fetch()
// itself resolves its promise with another, which looks up `then`.
function patchAll({ promises = true } = {}) {
  const targets = [
    [Promise.prototype, promises ? ['then', 'catch', 'finally'] : []],
    [String.prototype, ['indexOf', 'slice', 'charCodeAt']],
    [TextDecoder.prototype, ['decode']],
    [TextEncoder.prototype, ['encode']],
    [ReadableStream.prototype, ['getReader']],
    [ReadableStreamDefaultReader.prototype, ['read', 'cancel']],
    [Response.prototype, ['ok', 'status', 'headers', 'body', 'url']],
    [Headers.prototype, ['get']],
    [AbortController.prototype, ['abort', 'signal']],
    // Last, as the loops above iterate arrays.
    [Array.prototype, ['push', Symbol.iterator]],
  ];
  const restores = [];
  for (let i = 0; i < targets.length; i++) {
    restores[restores.length] = patch(targets[i][0], targets[i][1]);
  }
  return () => {
    for (let i = restores.length - 1; i >= 0; i--) restores[i]();
  };
}

// Messages as [data, lastEventId], recorded without the patched built-ins.
function recordMessages(source) {
  const messages = [];
  source.onmessage = (event) => {
    messages[messages.length] = [event.data, event.lastEventId];
  };
  return messages;
}

export const fromStreamUnderPollution = {
  async test() {
    const stream = streamOf(['id: 1\ndata: a\n', 'data: b\n\n', 'data: c\n\n']);
    const restore = patchAll({ promises: usingTsImpl || !usingTsStreams });
    let source;
    let messages;
    let ended;
    try {
      source = EventSource.from(stream);
      messages = recordMessages(source);
      ended = nextEvent(source, 'error');
    } finally {
      restore();
    }
    await ended;
    deepStrictEqual(messages, [
      ['a\nb', '1'],
      ['c', '1'],
    ]);
    strictEqual(source.readyState, EventSource.CLOSED);
  },
};

export const connectionUnderPollution = {
  async test(ctrl, env) {
    const restore = patchAll({ promises: false });
    let source;
    let messages;
    try {
      source = connect(env, '/last-event-id');
      messages = recordMessages(source);
      for (let i = 0; i < 100 && messages.length < 1; i++) {
        await scheduler.wait(10);
      }
      source.close();
    } finally {
      restore();
    }
    deepStrictEqual(messages, [['first', '1']]);
  },
};
