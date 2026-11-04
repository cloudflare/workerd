// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Reconnection after the end of a connected EventSource's stream: the
// 'error' event that announces it, the Last-Event-ID it sends, and the
// reconnection time a `retry` field sets. Divergences (see AGENTS.md):
// - #9: the TypeScript implementation sends the ID of the last dispatched
//   event, as the standard says; C++ sends the last `id` field parsed.
// - #16: the TypeScript implementation trims HTTP whitespace from the ID it
//   sends; C++ sends it as parsed.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { connect, nextEvent, record } from 'helpers';
import { usingTsImpl } from 'which-impl';

export const endOfStreamReconnects = {
  async test(ctrl, env) {
    const source = connect(env, '/messages');
    const events = record(source);
    const error = await nextEvent(source, 'error');
    strictEqual(error.error.message, 'The server disconnected.');
    strictEqual(source.readyState, EventSource.CONNECTING);
    await nextEvent(source, 'open');
    source.close();
    deepStrictEqual(events, [
      ['open', EventSource.OPEN],
      ['message', EventSource.OPEN],
      ['message', EventSource.OPEN],
      ['error', EventSource.CONNECTING, 'The server disconnected.'],
      ['open', EventSource.OPEN],
    ]);
  },
};

export const lastEventIdIsSent = {
  async test(ctrl, env) {
    const source = connect(env, '/last-event-id');
    const data = [];
    source.onmessage = (event) => data.push(event.data);
    while (data.length < 2) await nextEvent(source, 'message');
    source.close();
    deepStrictEqual(data, ['first', '1']);
  },
};

// #16.
export const lastEventIdWhitespace = {
  async test(ctrl, env) {
    const source = connect(env, '/padded-last-event-id');
    const data = [];
    source.onmessage = (event) => data.push(event.data);
    while (data.length < 2) await nextEvent(source, 'message');
    source.close();
    deepStrictEqual(data, ['first', usingTsImpl ? '[padded]' : '[ padded \t]']);
  },
};

// #9. WPT format-data-before-final-empty-line.any.js: the reconnection's
// message carries the last dispatched event's ID.
export const formatDataBeforeFinalEmptyLine = {
  async test(ctrl, env) {
    const source = connect(env, '/data-before-final-empty-line');
    const messages = [];
    source.onmessage = (event) => {
      messages.push([event.data, event.lastEventId]);
    };
    while (messages.length < 2) await nextEvent(source, 'message');
    source.close();
    deepStrictEqual(messages, [
      ['test1', ''],
      ['test1', usingTsImpl ? '' : 'test'],
    ]);
  },
};

// `retry: 1000` sets the reconnection time below the default two seconds.
// Only an upper bound is checked: the reconnection timer counts from the
// event loop's cached time, so it can fire early after a stall.
export const retrySetsTheReconnectionTime = {
  async test(ctrl, env) {
    const source = connect(env, '/retry');
    await nextEvent(source, 'error');
    const reconnected = await Promise.race([
      nextEvent(source, 'open').then(() => true),
      scheduler.wait(1900).then(() => false),
    ]);
    source.close();
    strictEqual(reconnected, true);
  },
};
