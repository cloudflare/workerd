// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';
import {
  createInstrumentationState,
  createTailStreamHandler,
  runInstrumentationTest,
} from 'instrumentation-test-helper';

// Create module-level state using the helper
const state = createInstrumentationState();

export default {
  tailStream: createTailStreamHandler(state),
};

export const test = {
  async test() {
    await runInstrumentationTest(state, expectedSpans, {
      testName: 'Images instrumentation',
      mapFn: (span) => {
        if (
          span['http.request.header.content-type'] &&
          span['http.request.header.content-type'].startsWith(
            'multipart/form-data; boundary='
          )
        ) {
          span['http.request.header.content-type'] =
            'multipart/form-data; boundary=<dynamic>';
        }
        return span;
      },
    });
  },
};

const expectedSpans = [
  {
    name: 'images_output',
    'cloudflare.images.options.format': 'image/avif',
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
    name: 'images_output',
    'cloudflare.images.options.format': 'image/avif',
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
    name: 'images_output',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'cloudflare.images.options.format': 'image/avif',
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
    name: 'images_output',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'cloudflare.images.options.format': 'image/avif',
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
    'http.response.body.size': 24661n,
    closed: true,
  },
  {
    name: 'images_output',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'cloudflare.images.options.format': 'image/avif',
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
    'http.response.body.size': 24664n,
    closed: true,
  },
  {
    name: 'images_output',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'cloudflare.images.options.format': 'image/avif',
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
    name: 'images_output',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'cloudflare.images.options.format': 'image/avif',
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
    'http.response.body.size': 86n,
    closed: true,
  },
  {
    name: 'images_output',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'cloudflare.images.options.format': 'image/avif',
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
    'http.response.body.size': 32855n,
    closed: true,
  },
  {
    name: 'images_output',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'cloudflare.images.options.format': 'image/avif',
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
    name: 'images_output',
    'cloudflare.images.options.format': 'image/avif',
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
    'http.response.body.size': 58n,
    closed: true,
  },
  {
    name: 'images_output',
    'cloudflare.images.options.format': 'image/avif',
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
    'http.response.body.size': 59n,
    closed: true,
  },
  {
    name: 'images_output',
    'cloudflare.images.options.format': 'image/avif',
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
    name: 'images_output',
    'cloudflare.images.options.format': 'image/avif',
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
    name: 'images_output',
    'cloudflare.images.options.format': 'image/avif',
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
    name: 'images_output',
    'cloudflare.images.options.format': 'image/avif',
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
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/info',
    'http.request.header.content-type':
      'multipart/form-data; boundary=<dynamic>',
    'http.response.status_code': 200n,
    'http.response.body.size': 63n,
    closed: true,
  },
  {
    name: 'images_info',
    'cloudflare.images.options.encoding': 'base64',
    'cloudflare.images.result.format': 'image/png',
    'cloudflare.images.result.file_size': 123,
    'cloudflare.images.result.width': 123,
    'cloudflare.images.result.height': 123,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/info',
    'http.request.header.content-type':
      'multipart/form-data; boundary=<dynamic>',
    'http.response.status_code': 200n,
    'http.response.body.size': 63n,
    closed: true,
  },
  {
    name: 'images_info',
    'cloudflare.images.options.encoding': 'base64',
    'cloudflare.images.result.format': 'image/png',
    'cloudflare.images.result.file_size': 123,
    'cloudflare.images.result.width': 123,
    'cloudflare.images.result.height': 123,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/info',
    'http.request.header.content-type':
      'multipart/form-data; boundary=<dynamic>',
    'http.response.status_code': 409n,
    'http.response.body.size': 22n,
    closed: true,
  },
  {
    name: 'images_info',
    'cloudflare.images.options.encoding': 'base64',
    'cloudflare.images.error.code': '123',
    'cloudflare.status': 'error',
    'error.type': 'ERROR 123: Bad request',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/info',
    'http.request.header.content-type':
      'multipart/form-data; boundary=<dynamic>',
    'http.response.status_code': 200n,
    'http.response.body.size': 26n,
    closed: true,
  },
  {
    name: 'images_info',
    'cloudflare.images.options.encoding': 'base64',
    'cloudflare.images.result.format': 'image/svg+xml',
    closed: true,
  },
  {
    name: 'images_output',
    'cloudflare.images.options.transforms':
      '[{"imageIndex":0,"rotate":90},{"imageIndex":1,"rotate":180},{"drawImageIndex":1,"targetImageIndex":0},{"drawImageIndex":3,"targetImageIndex":2},{"imageIndex":2,"rotate":270},{"drawImageIndex":2,"targetImageIndex":0},{"drawImageIndex":4,"targetImageIndex":0}]',
    'cloudflare.images.options.format': 'image/avif',
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
    name: 'images_output',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'cloudflare.images.options.format': 'image/avif',
    'cloudflare.images.options.anim': true,
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
    name: 'images_output',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'cloudflare.images.options.format': 'image/avif',
    'cloudflare.images.error.code': '123',
    'cloudflare.status': 'error',
    'error.type': 'ERROR 123: Bad request',
    closed: true,
  },
  {
    name: 'images_output',
    'cloudflare.images.options.transforms': '[{"imageIndex":0,"rotate":90}]',
    'cloudflare.images.options.format': 'image/avif',
    closed: true,
  },
  { name: 'images_output', closed: true },
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
  { name: 'images_output', closed: true },
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
