// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  validateHeaderName,
  validateHeaderValue,
} from 'node-internal:internal_http';
import { METHODS, STATUS_CODES } from 'node-internal:internal_http_constants';
import { ClientRequest } from 'node-internal:internal_http_client';
import { OutgoingMessage } from 'node-internal:internal_http_outgoing';
import { IncomingMessage } from 'node-internal:internal_http_incoming';
import { Agent, globalAgent } from 'node-internal:internal_http_agent';
import { Server, ServerResponse } from 'node-internal:internal_http_server';
import type { IncomingMessageCallback } from 'node-internal:internal_http_util';
import type { RequestOptions, ServerOptions, RequestListener } from 'node:http';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import { default as flags } from 'workerd:compatibility-flags';

export function request(
  url: string | URL | RequestOptions,
  options?: RequestOptions | IncomingMessageCallback,
  cb?: IncomingMessageCallback
): ClientRequest {
  return new ClientRequest(url, options, cb);
}

export function get(
  url: string | URL | RequestOptions,
  options?: RequestOptions | IncomingMessageCallback,
  cb?: IncomingMessageCallback
): ClientRequest {
  const req = request(url, options, cb);
  req.end();
  return req;
}

export function createServer(
  options: ServerOptions,
  handler: RequestListener
): Server {
  if (!flags.enableNodejsHttpServerModules) {
    throw new ERR_METHOD_NOT_IMPLEMENTED('createServer');
  }

  return new Server(options, handler);
}

export {
  validateHeaderName,
  validateHeaderValue,
  METHODS,
  STATUS_CODES,
  ClientRequest,
  OutgoingMessage,
  Agent,
  globalAgent,
  IncomingMessage,
  Server,
  ServerResponse,
};
export default {
  validateHeaderName,
  validateHeaderValue,
  METHODS,
  STATUS_CODES,
  ClientRequest,
  request,
  get,
  OutgoingMessage,
  Agent,
  globalAgent,
  IncomingMessage,
  Server,
  ServerResponse,
  createServer,
};
