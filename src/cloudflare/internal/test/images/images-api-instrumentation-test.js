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

// spans emitted by images-api-test.js in execution order
const expectedSpans = [
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.format': 'image/avif',
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
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.format': 'image/avif',
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
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.format': 'image/avif',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
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
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.format': 'image/avif',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
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
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.format': 'image/avif',
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
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.format': 'image/avif',
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
    'url.full': 'https://js.images.cloudflare.com/info',
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.encoding': 'base64',
    'cloudflare.images.result.file_size': 123,
    'cloudflare.images.result.format': 'image/png',
    'cloudflare.images.result.height': 123,
    'cloudflare.images.result.width': 123,
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
    'url.full': 'https://js.images.cloudflare.com/info',
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.encoding': 'base64',
    'cloudflare.images.result.file_size': 123,
    'cloudflare.images.result.format': 'image/png',
    'cloudflare.images.result.height': 123,
    'cloudflare.images.result.width': 123,
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
    'url.full': 'https://js.images.cloudflare.com/info',
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.error.code': '123',
    'cloudflare.images.options.encoding': 'base64',
    'error.type': 'ERROR 123: Bad request',
    'http.request.header.content-type':
      'multipart/form-data; boundary=<dynamic>',
    'http.response.status_code': 409n,
    'http.response.body.size': 22n,
    closed: true,
  },
  {
    name: 'fetch',
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.encoding': 'base64',
    'cloudflare.images.result.format': 'image/svg+xml',
    'http.request.header.content-type':
      'multipart/form-data; boundary=<dynamic>',
    'http.request.method': 'POST',
    'http.response.body.size': 26n,
    'http.response.status_code': 200n,
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'url.full': 'https://js.images.cloudflare.com/info',
    closed: true,
  },
  {
    name: 'fetch',
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.format': 'image/avif',
    'cloudflare.images.options.transforms':
      '[{"imageIndex":0,"rotate":90},{"imageIndex":1,"rotate":180},{"drawImageIndex":1,"targetImageIndex":0},{"drawImageIndex":3,"targetImageIndex":2},{"imageIndex":2,"rotate":270},{"drawImageIndex":2,"targetImageIndex":0},{"drawImageIndex":4,"targetImageIndex":0}]',
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
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.options.anim': true,
    'cloudflare.images.options.format': 'image/avif',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
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
    'cloudflare.binding.type': 'Images',
    'cloudflare.images.error.code': '123',
    'cloudflare.images.options.format': 'image/avif',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'error.type': 'ERROR 123: Bad request',
    'http.request.header.content-type':
      'multipart/form-data; boundary=<dynamic>',
    'http.response.status_code': 409n,
    'http.response.body.size': 22n,
    closed: true,
  },
  {
    name: 'images_output',
    'cloudflare.binding.type': 'Images',
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
    'cloudflare.binding.type': 'Images',
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
