// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

// tailStream is going to be invoked multiple times, but we want to wait
// to run the test until all executions are done. Collect promises for
// each
let invocationPromises = [];
let spans = new Map();

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
    // filtering spans not associated with KV
    let received = Array.from(spans.values()).filter(
      (span) => span.name !== 'jsRpcSession'
    );

    // spans emitted by kv-test.js in execution order
    let expected = [
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------583079568443922091480339',
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
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------917895793710282367115997',
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
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------274020163342001891217006',
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
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------016965139147113097012573',
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
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------800533565867149828727514',
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
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------788884220072645140944057',
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
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------076772162047566398596413',
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
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------683302700719873337688257',
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
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------638268216708834840270438',
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
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------467138828766648388569910',
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
        'http.request.header.content-type':
          'multipart/form-data; boundary=--------------------------165527776325986498384287',
        'http.response.status_code': 200n,
        'http.response.body.size': 60n,
        closed: true,
      },
    ];

    for (const each of received) {
      delete each['http.request.header.content-type'];
    }

    for (const each of expected) {
      delete each['http.request.header.content-type'];
    }

    assert.deepStrictEqual(received, expected);
  },
};
