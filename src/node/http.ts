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
import {
  Server,
  ServerResponse,
  _connectionListener,
} from 'node-internal:internal_http_server';
import { validateInteger } from 'node-internal:validators';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import type { IncomingMessageCallback } from 'node-internal:internal_http_util';
import type { RequestOptions, ServerOptions, RequestListener } from 'node:http';

const enableNodejsHttpServerModules =
  !!Cloudflare.compatibilityFlags['enable_nodejs_http_server_modules'];

// TODO(soon): Our global implementation of WebSocket does not match
// Node.js' implementation which is compliant to the spec. However,
// We were previously relying on the unenv polyfills for this, and
// it just re-exported the global also, so this shouldn't be a breaking
// change. Later, however, we'll need to reconcile the Node.js and
// standard spec conformance here.
export const WebSocket = globalThis.WebSocket;
export const CloseEvent = globalThis.CloseEvent;
export const MessageEvent = globalThis.MessageEvent;

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
  if (!enableNodejsHttpServerModules) {
    throw new ERR_METHOD_NOT_IMPLEMENTED('createServer');
  }

  return new Server(options, handler);
}

// the maximum size of HTTP headers (default: 16384 (16KB))
// ref: https://github.com/nodejs/node/blob/3b715d35440d509c6242ea61dec9d2802f219c83/src/node_options.cc#L787
export const maxHeaderSize = 16384;

export function setMaxIdleHTTPParsers(max: unknown): void {
  validateInteger(max, 'max', 1);
  throw new ERR_METHOD_NOT_IMPLEMENTED('setMaxIdleHTTPParsers');
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
  _connectionListener,
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
  maxHeaderSize,
  setMaxIdleHTTPParsers,
  _connectionListener,
  WebSocket,
  CloseEvent,
  MessageEvent,
};
