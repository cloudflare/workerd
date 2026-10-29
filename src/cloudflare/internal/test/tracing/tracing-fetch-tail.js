// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

const invocations = [];

export default {
  tailStream() {
    const { promise, resolve } = Promise.withResolvers();
    const invocation = { ended: promise, spans: new Map(), errors: [] };
    invocations.push(invocation);
    return ({ event, spanContext }) => {
      if (event.type === 'spanOpen') {
        assert.equal(event.name, 'fetch');
        invocation.spans.set(event.spanId, {
          attributes: {},
          exceptions: [],
          statuses: [],
          closed: false,
        });
      } else if (event.type === 'outcome') {
        assert.equal(event.outcome, 'ok');
        resolve();
      } else if (event.type === 'log') {
        if (event.message[0] === 'fetch-error') {
          invocation.errors.push(event.message.slice(1));
        }
      } else {
        const span = invocation.spans.get(spanContext.spanId);
        if (!span) return;
        assert.equal(
          span.closed,
          false,
          `${event.type} arrived after spanClose`
        );
        if (event.type === 'attributes') {
          for (const { name, value } of event.info) {
            span.attributes[name] = value;
          }
        } else if (event.type === 'exception') {
          span.exceptions.push(event);
        } else if (event.type === 'spanUpdate') {
          span.statuses.push(event.info);
        } else if (event.type === 'spanClose') {
          span.closed = true;
        }
      }
    };
  },
};

export const validate = {
  async test() {
    await Promise.all(invocations.map(({ ended }) => ended));
    const spans = invocations.flatMap(({ spans }) => [...spans.values()]);
    assert.equal(spans.length, 8);
    const expected = new Map([
      ['http://origin/success', 200],
      ['http://origin/start?hop=1', 302],
      ['http://origin/middle?hop=2', 307],
      ['http://other/final?hop=3', 200],
      ['http://origin/start?manual=1', 302],
      ['http://origin/status/404', 404],
      ['http://origin/status/503', 503],
      ['http://origin/unreachable', undefined],
    ]);
    for (const span of spans) {
      const url = span.attributes['url.full'];
      assert.ok(expected.has(url), `unexpected or duplicate fetch URL: ${url}`);
      const status = expected.get(url);
      expected.delete(url);
      assert.ok(span.closed);
      assert.equal(span.attributes['http.request.method'], 'GET');
      assert.equal(
        span.attributes['http.response.status_code'],
        status === undefined ? undefined : BigInt(status)
      );
      if (status === undefined) {
        assert.equal(span.attributes['error.type'], 'Error');
        assert.deepEqual(span.statuses, [
          { type: 'status', status: { code: 'error' } },
        ]);
        assert.equal(span.exceptions.length, 1);
        const [name, message] = invocations.flatMap(({ errors }) => errors)[0];
        assert.equal(span.exceptions[0].name, name);
        assert.equal(span.exceptions[0].message, message);
      } else {
        assert.equal(
          span.attributes['error.type'],
          status >= 400 ? String(status) : undefined
        );
        assert.equal(span.exceptions.length, 0);
        assert.equal(span.statuses.length, 0);
      }
    }
    assert.equal(expected.size, 0);
  },
};
