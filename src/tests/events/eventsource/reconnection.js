// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Reconnection after the end of a connected EventSource's stream: the
// 'error' event that announces it, the Last-Event-ID it sends, and the
// reconnection time a `retry` field sets. Divergence #9 (see AGENTS.md):
// the TypeScript implementation sends the ID of the last dispatched event,
// as the standard says; C++ sends the last `id` field parsed.

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
export const retrySetsTheReconnectionTime = {
  async test(ctrl, env) {
    const source = connect(env, '/retry');
    let opens = 0;
    source.onopen = () => opens++;
    await nextEvent(source, 'error');
    await scheduler.wait(1500);
    source.close();
    strictEqual(opens, 2);
  },
};
