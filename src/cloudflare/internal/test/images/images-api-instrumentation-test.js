// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

// Images test uses a different keying strategy (traceId-based)
// So we need a custom collector
const invocationPromises = [];
const spans = new Map();

export default {
  tailStream(event, env, ctx) {
    // For each "onset" event, store a promise which we will resolve when
    // we receive the equivalent "outcome" event
    let resolveFn;
    invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    // Accumulate the span info for easier testing
    return (event) => {
      switch (event.event.type) {
        case 'spanOpen':
          // The span ids will change between tests, but Map preserves insertion order
          spans.set(event.spanContext.traceId, { name: event.event.name });
          break;
        case 'attributes': {
          let span = spans.get(event.spanContext.traceId);
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          spans.set(event.spanContext.traceId, span);
          break;
        }
        case 'spanClose': {
          let span = spans.get(event.spanContext.traceId);
          span['closed'] = true;
          spans.set(event.spanContext.traceId, span);
          break;
        }
        case 'outcome':
          resolveFn();
          break;
      }
    };
  },
};

export const test = {
  async test() {
    // Wait for all the tailStream executions to finish
    await Promise.allSettled(invocationPromises);

    // Recorded streaming tail worker events, in insertion order,
    // filtering spans not associated with Images
    let received = Array.from(spans.values()).filter(
      (span) => span.name !== 'jsRpcSession'
    );

    // spans emitted by images-api-test.js in execution order
    let expected = [
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 200n,
        'http.response.body.size': 63n,
        closed: true,
      },
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 200n,
        'http.response.body.size': 57n,
        closed: true,
      },
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 200n,
        'http.response.body.size': 491605n,
        closed: true,
      },
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 200n,
        'http.response.body.size': 655465n,
        closed: true,
      },
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 200n,
        'http.response.body.size': 60n,
        closed: true,
      },
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 200n,
        'http.response.body.size': 63n,
        closed: true,
      },
      {
        name: 'images_info',
        'cloudflare.images.info.format': 'image/png',
        'cloudflare.images.info.file_size': 123,
        'cloudflare.images.info.width': 123,
        'cloudflare.images.info.height': 123,
        closed: true,
      },
      {
        name: 'images_info',
        'cloudflare.images.info.encoding': 'base64',
        'cloudflare.images.info.format': 'image/png',
        'cloudflare.images.info.file_size': 123,
        'cloudflare.images.info.width': 123,
        'cloudflare.images.info.height': 123,
        closed: true,
      },
      { name: 'images_info', closed: true },
      {
        name: 'images_info',
        'cloudflare.images.info.format': 'image/svg+xml',
        closed: true,
      },
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 200n,
        'http.response.body.size': 359n,
        closed: true,
      },
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 200n,
        'http.response.body.size': 102n,
        closed: true,
      },
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 409n,
        'http.response.body.size': 22n,
        closed: true,
      },
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 200n,
        'http.response.body.size': 88n,
        closed: true,
      },
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.response.status_code': 200n,
        'http.response.body.size': 60n,
        closed: true,
      },
    ];

    // Validate and normalize content-type headers (they contain dynamic boundaries)
    for (const each of received) {
      const contentType = each['http.request.header.content-type'];
      if (contentType) {
        // Verify it starts with the expected prefix
        assert.ok(
          contentType.startsWith('multipart/form-data; boundary='),
          `Expected multipart/form-data content-type, got: ${contentType}`
        );
        // Normalize to a standard value for comparison
        each['http.request.header.content-type'] =
          'multipart/form-data; boundary=<dynamic>';
      }
    }

    // Add normalized content-type to expected spans
    for (const each of expected) {
      if (
        each.name === 'fetch' &&
        each['url.full'] === 'https://js.images.cloudflare.com/transform'
      ) {
        each['http.request.header.content-type'] =
          'multipart/form-data; boundary=<dynamic>';
      }
    }

    assert.deepStrictEqual(received, expected);
  },
};
