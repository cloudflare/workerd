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
import { Agent, globalAgent } from 'node-internal:internal_http_agent';
import type { IncomingMessageCallback } from 'node-internal:internal_http_util';
import type { RequestOptions } from 'node:http';

const { WebSocket } = globalThis;

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

export {
  validateHeaderName,
  validateHeaderValue,
  METHODS,
  STATUS_CODES,
  ClientRequest,
  WebSocket,
  OutgoingMessage,
  Agent,
  globalAgent,
};
export default {
  validateHeaderName,
  validateHeaderValue,
  METHODS,
  STATUS_CODES,
  ClientRequest,
  request,
  get,
  WebSocket,
  OutgoingMessage,
  Agent,
  globalAgent,
};
