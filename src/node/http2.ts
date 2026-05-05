// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.
//
// Portions of this code are based on unenv https://github.com/unjs/unenv/blob/main/LICENSE
// MIT License, Copyright (c) Pooya Parsa <pooya@pi0.io>

// We currently have no intention of implementing HTTP/2 in workerd but we want to
// provide the non-op stubs so that polyfills are not necessary for modules that
// unconditionally import 'http2'.

import type H2 from 'node:http2';
import type H1 from 'node:http';

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import { Buffer } from 'node-internal:internal_buffer';

import { constants } from 'node-internal:internal_http2_constants';

export { constants };

// eslint-disable-next-line @typescript-eslint/no-unnecessary-type-parameters
function notImplementedClass<T = unknown>(name: string): T {
  return class {
    constructor() {
      throw new ERR_METHOD_NOT_IMPLEMENTED(name);
    }
  } as T;
}

export function createSecureServer(..._args: unknown[]): H2.Http2SecureServer {
  throw new ERR_METHOD_NOT_IMPLEMENTED('http2.createSecureServer');
}

export function createServer(..._args: unknown[]): H2.Http2Server {
  throw new ERR_METHOD_NOT_IMPLEMENTED('http2.createServer');
}

export function connect(..._args: unknown[]): H2.ClientHttp2Session {
  throw new ERR_METHOD_NOT_IMPLEMENTED('http2.connect');
}

export function performServerHandshake<
  Http1Request extends typeof H1.IncomingMessage = typeof H1.IncomingMessage,
  Http1Response extends typeof H1.ServerResponse<InstanceType<Http1Request>> =
    typeof H1.ServerResponse,
  Http2Request extends typeof H2.Http2ServerRequest =
    typeof H2.Http2ServerRequest,
  Http2Response extends typeof H2.Http2ServerResponse<
    InstanceType<Http2Request>
  > = typeof H2.Http2ServerResponse,
>(
  ..._args: unknown[]
): H2.ServerHttp2Session<
  Http1Request,
  Http1Response,
  Http2Request,
  Http2Response
> {
  throw new ERR_METHOD_NOT_IMPLEMENTED('http2.performServerHandshake');
}

export const Http2ServerRequest: H2.Http2ServerRequest = notImplementedClass(
  'http2.Http2ServerRequest'
);

export const Http2ServerResponse: H2.Http2ServerResponse = notImplementedClass(
  'http2.Http2ServerResponse'
);

export function getDefaultSettings(..._args: unknown[]): H2.Settings {
  // We really ought to throw here but the unenv polyfill returns an object
  // so let's do so also.
  return Object.create({
    headerTableSize: 4096,
    enablePush: true,
    initialWindowSize: 65_535,
    maxFrameSize: 16_384,
    maxConcurrentStreams: 4_294_967_295,
    maxHeaderSize: 65_535,
    maxHeaderListSize: 65_535,
    enableConnectProtocol: false,
  }) as H2.Settings;
}

export function getPackedSettings(..._args: unknown[]): Buffer {
  return Buffer.alloc(0);
}

export function getUnpackedSettings(..._args: unknown[]): H2.Settings {
  return {};
}

export const sensitiveHeaders = Symbol('nodejs.http2.sensitiveHeaders');

export default {
  constants,
  createSecureServer,
  createServer,
  Http2ServerRequest,
  Http2ServerResponse,
  connect,
  getDefaultSettings,
  getPackedSettings,
  getUnpackedSettings,
  performServerHandshake,
  sensitiveHeaders,
};
