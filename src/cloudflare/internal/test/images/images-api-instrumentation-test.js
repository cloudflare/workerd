// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TODO(cleanup): Refactor to use one of the shared tail workers.

import {
  createInstrumentationState,
  runInstrumentationTest,
} from 'instrumentation-test-helper';

// Images test uses a different keying strategy (traceId-based)
// So we need a custom tailStream handler
const state = createInstrumentationState();

export default {
  tailStream(event, env, ctx) {
    // For each "onset" event, store a promise which we will resolve when
    // we receive the equivalent "outcome" event
    let resolveFn;
    state.invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    // Accumulate the span info for easier testing
    return (event) => {
      switch (event.event.type) {
        case 'spanOpen':
          // The span ids will change between tests, but Map preserves insertion order
          // Note: Images uses traceId instead of spanId for keying
          state.spans.set(event.spanContext.traceId, {
            name: event.event.name,
          });
          break;
        case 'attributes': {
          let span = state.spans.get(event.spanContext.traceId);
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          state.spans.set(event.spanContext.traceId, span);
          break;
        }
        case 'spanClose': {
          let span = state.spans.get(event.spanContext.traceId);
          span['closed'] = true;
          state.spans.set(event.spanContext.traceId, span);
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
    // spans emitted by images-api-test.js in execution order
    const expectedSpans = [
      {
        name: 'fetch',
        'network.protocol.name': 'http',
        'network.protocol.version': 'HTTP/1.1',
        'http.request.method': 'POST',
        'url.full': 'https://js.images.cloudflare.com/transform',
        'http.request.header.content-type':
          'multipart/form-data; boundary=<dynamic>',
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
          'multipart/form-data; boundary=<dynamic>',
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
          'multipart/form-data; boundary=<dynamic>',
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
          'multipart/form-data; boundary=<dynamic>',
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
          'multipart/form-data; boundary=<dynamic>',
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
          'multipart/form-data; boundary=<dynamic>',
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
          'multipart/form-data; boundary=<dynamic>',
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
          'multipart/form-data; boundary=<dynamic>',
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
          'multipart/form-data; boundary=<dynamic>',
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
          'multipart/form-data; boundary=<dynamic>',
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
          'multipart/form-data; boundary=<dynamic>',
        'http.response.status_code': 200n,
        'http.response.body.size': 60n,
        closed: true,
      },
    ];

    // Use the helper with mapFn to normalize dynamic multipart boundaries
    await runInstrumentationTest(state, expectedSpans, {
      testName: 'Images instrumentation',
      mapFn: (span) => {
        const contentType = span['http.request.header.content-type'];
        if (contentType?.startsWith('multipart/form-data; boundary=')) {
          return {
            ...span,
            'http.request.header.content-type':
              'multipart/form-data; boundary=<dynamic>',
          };
        }
        return span;
      },
    });
  },
};
