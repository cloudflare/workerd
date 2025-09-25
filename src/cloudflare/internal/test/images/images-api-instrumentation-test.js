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
          'multipart/form-data; boundary=--------------------------310253009656403672722808',
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
          'multipart/form-data; boundary=--------------------------721037829633019043454026',
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
          'multipart/form-data; boundary=--------------------------953568754209602608781375',
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
          'multipart/form-data; boundary=--------------------------440652620341578841384574',
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
          'multipart/form-data; boundary=--------------------------134462238417857686794018',
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
          'multipart/form-data; boundary=--------------------------451520024689691544877699',
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
          'multipart/form-data; boundary=--------------------------549443388595697638206735',
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
          'multipart/form-data; boundary=--------------------------827110864910850091668521',
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
          'multipart/form-data; boundary=--------------------------663260270205734226709932',
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
          'multipart/form-data; boundary=--------------------------302484619284257147072706',
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
          'multipart/form-data; boundary=--------------------------775478735866429733305436',
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
