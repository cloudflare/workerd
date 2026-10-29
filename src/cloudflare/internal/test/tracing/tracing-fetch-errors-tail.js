// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

const invocations = new Map();

export default {
  tailStream(onset) {
    const { promise: ended, resolve } = Promise.withResolvers();
    // Raw events preserve ordering and exceptions, which the shared span collector discards.
    // Assertions run in validate so a failed assertion cannot prevent outcome collection.
    const events = [];
    invocations.set(onset.event.entrypoint, { ended, events });
    return (item) => {
      events.push(item);
      if (item.event.type === 'outcome') resolve();
    };
  },
};

function collectSpans(events) {
  const spans = new Map();
  for (const { event, spanContext } of events) {
    if (event.type === 'spanOpen') {
      assert.equal(event.name, 'fetch');
      spans.set(event.spanId, {
        attributes: {},
        statuses: [],
        closed: false,
      });
    } else if (event.type === 'outcome') {
      assert.equal(event.outcome, 'ok');
    } else if (event.type === 'exception') {
      assert.fail('fetch failures must not generate exception events');
    } else {
      const span = spans.get(spanContext.spanId);
      if (!span) continue;
      assert.equal(span.closed, false, `${event.type} arrived after spanClose`);
      if (event.type === 'attributes') {
        for (const { name, value } of event.info) span.attributes[name] = value;
      } else if (event.type === 'spanUpdate') {
        span.statuses.push(event.info);
      } else if (event.type === 'spanClose') {
        span.closed = true;
      }
    }
  }
  return [...spans.values()];
}

function assertError(span, name, message) {
  assert.equal(span.attributes['error.type'], name);
  assert.equal(span.statuses.length, 1);
  assert.equal(span.statuses[0].type, 'status');
  assert.equal(span.statuses[0].status.code, 'error');
  if (message instanceof RegExp) {
    assert.match(span.statuses[0].status.message, message);
  } else {
    assert.equal(span.statuses[0].status.message, message);
  }
}

function assertUnset(span) {
  assert.equal(span.attributes['error.type'], undefined);
  assert.equal(span.statuses.length, 0);
}

export const validate = {
  async test() {
    await Promise.all([...invocations.values()].map(({ ended }) => ended));
    const spansFor = (test) => {
      const invocation = invocations.get(test);
      assert.ok(invocation, `missing invocation: ${test}`);
      const spans = collectSpans(invocation.events);
      assert.ok(spans.every(({ closed }) => closed));
      return spans;
    };
    for (const test of ['networkFailure', 'noUserGetters']) {
      const spans = spansFor(test);
      assert.equal(spans.length, 1);
      assertError(spans[0], 'Error', 'Network connection lost.');
    }
    for (const test of [
      'abort',
      'manualRedirect',
      'success',
      'webSocketFailure',
    ]) {
      const spans = spansFor(test);
      assert.equal(spans.length, 1);
      assertUnset(spans[0]);
    }
    const invalid = spansFor('invalidLocation');
    assert.equal(invalid.length, 1);
    assertError(
      invalid[0],
      'TypeError',
      'Invalid Location header; unable to follow redirect.'
    );
    assert.equal(invalid[0].attributes['http.response.status_code'], 302n);

    const later = spansFor('laterHopFailure');
    assert.equal(later.length, 2);
    assertUnset(later[0]);
    assertError(
      later[1],
      'TypeError',
      'Invalid Location header; unable to follow redirect.'
    );

    const loop = spansFor('tooManyRedirects');
    assert.equal(loop.length, 21);
    loop.slice(0, -1).forEach(assertUnset);
    assertError(loop.at(-1), 'TypeError', /^Too many redirects\./);
    assert.equal(loop.at(-1).attributes['http.response.status_code'], 302n);

    const long = spansFor('longRedirectError');
    assert.equal(long.length, 21);
    long.slice(0, -1).forEach(assertUnset);
    assertError(long.at(-1), 'TypeError', /^Too many redirects\./);
    assert.ok(long.at(-1).statuses[0].status.message.length <= 4096);
  },
};
