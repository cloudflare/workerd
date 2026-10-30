// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// How an EventSource checks the response: a status or content type it does
// not accept fails it for good with an AbortError, a redirect moves its
// URL, and a response without a body is retried once.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { connect, nextEvent, record } from 'helpers';

async function failure(env, path) {
  const source = connect(env, path);
  const events = record(source);
  const event = await nextEvent(source, 'error');
  await scheduler.wait(10);
  strictEqual(source.readyState, EventSource.CLOSED);
  strictEqual(events.length, 1);
  strictEqual(event.error.name, 'AbortError');
  return event.error.message;
}

export const statusMustBeOk = {
  async test(ctrl, env) {
    strictEqual(
      await failure(env, '/status?s=500'),
      'The response status code was 500.'
    );
  },
};

export const contentTypeMustBeEventStream = {
  async test(ctrl, env) {
    for (const contentType of ['text/plain', 'bogus']) {
      strictEqual(
        await failure(
          env,
          `/content-type?ct=${encodeURIComponent(contentType)}`
        ),
        `The content type '${contentType}' is invalid.`
      );
    }
    strictEqual(
      await failure(env, '/no-content-type'),
      'No content type header was present in the response.'
    );
    // Parameters are allowed.
    const source = connect(
      env,
      '/content-type?ct=' +
        encodeURIComponent('Text/Event-Stream; charset=utf-8')
    );
    strictEqual((await nextEvent(source, 'message')).data, 'x');
    source.close();
  },
};

// A redirect moves the EventSource's URL, and its messages' origin.
export const redirectMovesTheUrl = {
  async test(ctrl, env) {
    const source = connect(env, '/redirect');
    const event = await nextEvent(source, 'message');
    strictEqual(source.url, 'http://other.example/request-headers');
    strictEqual(event.origin, 'http://other.example');
    source.close();
  },
};

// A response without a body is retried once, as an end of the stream;
// a second one in a row fails the EventSource.
export const responseWithoutABody = {
  async test(ctrl, env) {
    const source = connect(env, '/reconnect-fail');
    const events = record(source);
    while (source.readyState !== EventSource.CLOSED) {
      await nextEvent(source, 'error');
    }
    deepStrictEqual(events, [
      ['open', EventSource.OPEN],
      ['message', EventSource.OPEN],
      ['error', EventSource.CONNECTING, 'The server disconnected.'],
      ['open', EventSource.OPEN],
      ['message', EventSource.OPEN],
      ['error', EventSource.CONNECTING, 'The server disconnected.'],
      [
        'error',
        EventSource.CONNECTING,
        'The server provided no content. Will try reconnecting.',
      ],
      ['error', EventSource.CLOSED, 'The server provided no content.'],
    ]);
  },
};
