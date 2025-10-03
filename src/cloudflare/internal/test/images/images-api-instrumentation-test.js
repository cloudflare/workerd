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
      logReceived: true,
    });
  },
};

const expectedSpans = [
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------615965590899103145492265',
    'http.response.status_code': '200',
    'http.response.body.size': '63',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------167586180047662686736676',
    'http.response.status_code': '200',
    'http.response.body.size': '57',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------579010917079567072179546',
    'http.response.status_code': '200',
    'http.response.body.size': '88',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------977629254377626964298223',
    'http.response.status_code': '200',
    'http.response.body.size': '24661',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------039731325327526865078824',
    'http.response.status_code': '200',
    'http.response.body.size': '24664',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------737892314505307427464222',
    'http.response.status_code': '200',
    'http.response.body.size': '491605',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------719823474645062940929850',
    'http.response.status_code': '200',
    'http.response.body.size': '86',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------849924865791231862626402',
    'http.response.status_code': '200',
    'http.response.body.size': '32855',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------413584755719346458025832',
    'http.response.status_code': '200',
    'http.response.body.size': '655465',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------947042092500640554185656',
    'http.response.status_code': '200',
    'http.response.body.size': '58',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------818247180768484016817816',
    'http.response.status_code': '200',
    'http.response.body.size': '59',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------761923912773107621422064',
    'http.response.status_code': '200',
    'http.response.body.size': '60',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------703411003172892807209064',
    'http.response.status_code': '200',
    'http.response.body.size': '63',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------858470788927117308922087',
    'http.response.status_code': '200',
    'http.response.body.size': '63',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------183692894207653242528151',
    'http.response.status_code': '200',
    'http.response.body.size': '63',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/info',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------691613860007854207499098',
    'http.response.status_code': '200',
    'http.response.body.size': '63',
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
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/info',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------484070132352529553184660',
    'http.response.status_code': '200',
    'http.response.body.size': '63',
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
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/info',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------153409389481542561802082',
    'http.response.status_code': '409',
    'http.response.body.size': '22',
    closed: true,
  },
  {
    name: 'images_info',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/info',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------603676565486638582793689',
    'http.response.status_code': '200',
    'http.response.body.size': '26',
    closed: true,
  },
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
      'multipart/form-data; boundary=--------------------------871688255865353372976196',
    'http.response.status_code': '200',
    'http.response.body.size': '359',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------440389984384468672397025',
    'http.response.status_code': '200',
    'http.response.body.size': '102',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------616060565849662757637711',
    'http.response.status_code': '409',
    'http.response.body.size': '22',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------713310131679796044938178',
    'http.response.status_code': '200',
    'http.response.body.size': '88',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'https://js.images.cloudflare.com/transform',
    'http.request.header.content-type':
      'multipart/form-data; boundary=--------------------------940322800708440200398602',
    'http.response.status_code': '200',
    'http.response.body.size': '60',
    closed: true,
  },
];
