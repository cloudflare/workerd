// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  validateHeaderName,
  validateHeaderValue,
} from 'node-internal:internal_http';
import { METHODS, STATUS_CODES } from 'node-internal:internal_http_constants';
import { OutgoingMessage } from 'node-internal:internal_http_outgoing';
import { ClientRequest } from 'node-internal:internal_http_client';
import { Agent, globalAgent } from 'node-internal:internal_http_agent';

import type { RequestOptions } from 'node:http';

const { WebSocket } = globalThis;

export function request(
  url: string | URL,
  options: RequestOptions,
  cb: (err: Error | null) => void
) {
  return new ClientRequest(url, options, cb);
}

export function get(
  url: string | URL,
  options: RequestOptions,
  cb: (err: Error | null) => void
) {
  const req = request(url, options, cb);
  req.end();
  return req;
}

export {
  validateHeaderName,
  validateHeaderValue,
  METHODS,
  STATUS_CODES,
  OutgoingMessage,
  ClientRequest,
  WebSocket,
  Agent,
  globalAgent,
};
export default {
  validateHeaderName,
  validateHeaderValue,
  METHODS,
  STATUS_CODES,
  OutgoingMessage,
  ClientRequest,
  request,
  get,
  WebSocket,
  Agent,
  globalAgent,
};
