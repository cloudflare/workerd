// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests the config's `tracing.otlp`: the spans of an invocation are POSTed to the designated
// service as an OTLP/HTTP protobuf request.
import * as assert from 'node:assert';

const SPAN_KIND_SERVER = 2;
const SPAN_KIND_CLIENT = 3;
const STATUS_CODE_ERROR = 2;

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname === '/leaf') {
      return new Response('leaf');
    }
    await ctx.tracing.enterSpan('my-span', async (span) => {
      span.setAttribute('answer', 42);
      span.setAttribute('ratio', 0.5);
      span.setAttribute('cached', false);
      await (await env.SELF.fetch('http://traced/leaf?secret=1')).text();
    });
    return new Response('not here', { status: 503 });
  },
};

// The requests the collector has received, once there are at least `count` of them. Spans are
// posted after the response that ends their invocation, so the test has to wait for them.
async function collected(env, count) {
  for (;;) {
    const response = await env.COLLECTOR.fetch('http://collector/requests');
    const requests = await response.json();
    if (requests.length >= count) return requests;
    await scheduler.wait(10);
  }
}

export const test = {
  async test(ctrl, env) {
    const response = await env.SELF.fetch('http://traced/outer?x=1');
    assert.strictEqual(response.status, 503);
    await response.text();

    // One request per invocation: the nested /leaf invocation finishes first.
    const requests = await collected(env, 2);
    assert.strictEqual(requests.length, 2);
    const [leaf, outer] = requests;
    for (const request of requests) {
      assert.strictEqual(request.contentType, 'application/x-protobuf');
      assert.deepStrictEqual(request.resource, {
        'service.name': 'traced',
        'telemetry.sdk.name': 'workers-runtime',
        'telemetry.sdk.language': 'js',
      });
    }

    assert.deepStrictEqual(
      outer.spans.map((span) => span.name),
      ['fetch', 'my-span', 'GET']
    );
    const [fetch, mySpan, root] = outer.spans;

    // The root describes the invocation. A 5xx response makes it an error.
    assert.strictEqual(root.kind, SPAN_KIND_SERVER);
    assert.strictEqual(root.parentSpanId, undefined);
    assert.strictEqual(root.attributes['faas.trigger'], 'http');
    assert.strictEqual(root.attributes['http.request.method'], 'GET');
    assert.strictEqual(root.attributes['url.full'], 'http://traced/outer?x=1');
    assert.strictEqual(root.attributes['url.path'], '/outer');
    assert.strictEqual(root.attributes['url.query'], 'x=1');
    assert.strictEqual(root.attributes['server.address'], 'traced');
    assert.strictEqual(root.attributes['http.response.status_code'], 503);
    assert.strictEqual(root.attributes['cloudflare.outcome'], 'ok');
    assert.strictEqual(
      root.attributes['cloudflare.invocation.sequence.number'],
      1
    );
    assert.deepStrictEqual(root.status, {
      code: STATUS_CODE_ERROR,
      message: '',
    });
    assert.strictEqual(root.attributes['error.type'], '503');

    // A span the worker opened, with each kind of attribute value.
    assert.strictEqual(mySpan.parentSpanId, root.spanId);
    assert.strictEqual(mySpan.traceId, root.traceId);
    assert.strictEqual(mySpan.attributes.answer, 42);
    assert.strictEqual(mySpan.attributes.ratio, 0.5);
    assert.strictEqual(mySpan.attributes.cached, false);
    assert.strictEqual(mySpan.status, undefined);

    // The subrequest made inside it.
    assert.strictEqual(fetch.kind, SPAN_KIND_CLIENT);
    assert.strictEqual(fetch.parentSpanId, mySpan.spanId);
    assert.strictEqual(
      fetch.attributes['url.full'],
      'http://traced/leaf?secret=1'
    );
    assert.strictEqual(fetch.attributes['url.query'], 'secret=1');

    // The invocation the subrequest started continues the trace under the fetch span.
    assert.deepStrictEqual(
      leaf.spans.map((span) => span.name),
      ['GET']
    );
    assert.strictEqual(leaf.spans[0].traceId, root.traceId);
    assert.strictEqual(leaf.spans[0].parentSpanId, fetch.spanId);
    assert.strictEqual(leaf.spans[0].status, undefined);
  },
};
