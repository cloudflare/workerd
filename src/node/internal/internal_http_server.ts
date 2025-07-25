// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

// This module implements Node.js-compatible HTTP server functionality on top of
// the fetch API due to workerd limitations. The key challenge is bridging Node.js's
// stream-based API with the Fetch API's Request/Response model.
//
// The ServerResponse class implements a single-buffer strategy to minimize memory
// usage when converting from Node.js streams to Fetch Response bodies:
// - Pre-header data is temporarily buffered until headers are sent
// - Post-header data streams directly without intermediate buffering
// - Memory is freed immediately after the transition point

import {
  ERR_METHOD_NOT_IMPLEMENTED,
  ERR_HTTP_HEADERS_SENT,
  ERR_HTTP_INVALID_STATUS_CODE,
  ERR_INVALID_CHAR,
  ERR_INVALID_ARG_VALUE,
  ERR_OUT_OF_RANGE,
  ERR_OPTION_NOT_IMPLEMENTED,
  ERR_SERVER_ALREADY_LISTEN,
} from 'node-internal:internal_errors';
import { EventEmitter } from 'node-internal:events';
import {
  kUniqueHeaders,
  OutgoingMessage,
  parseUniqueHeadersOption,
} from 'node-internal:internal_http_outgoing';
import {
  validateBoolean,
  validateInteger,
  validateObject,
  validatePort,
} from 'node-internal:validators';
import { portMapper } from 'cloudflare-internal:http';
import { IncomingMessage } from 'node-internal:internal_http_incoming';
import { STATUS_CODES } from 'node-internal:internal_http_constants';
import {
  kServerResponse,
  kIncomingMessage,
} from 'node-internal:internal_http_util';
import {
  kOutHeaders,
  WrittenDataBufferEntry,
  HeadersSentEvent,
} from 'node-internal:internal_http_outgoing';
import {
  chunkExpression,
  _checkInvalidHeaderChar,
} from 'node-internal:internal_http';
import { _normalizeArgs } from 'node-internal:internal_net';
import { Buffer } from 'node-internal:internal_buffer';

import type {
  Server as _Server,
  ServerResponse as _ServerResponse,
  RequestListener,
  ServerOptions,
  OutgoingHttpHeaders,
  OutgoingHttpHeader,
} from 'node:http';
import type { Socket, AddressInfo } from 'node:net';
import { default as flags } from 'workerd:compatibility-flags';

export const kConnectionsCheckingInterval = Symbol(
  'http.server.connectionsCheckingInterval'
);

export type StreamController = ReadableStreamDefaultController<Uint8Array>;

export type DataWrittenEvent = {
  index: number;
  entry: WrittenDataBufferEntry;
};

export class Server
  extends EventEmitter
  implements _Server, BaseWithHttpOptions
{
  // @ts-expect-error TS2416 Server is not assignable to same property in base type.
  [kIncomingMessage]: typeof IncomingMessage = IncomingMessage;
  // @ts-expect-error TS2416 Server is not assignable to same property in base type.
  [kServerResponse]: typeof ServerResponse = ServerResponse;
  [kConnectionsCheckingInterval]?: number;
  [kUniqueHeaders]: Set<string> | null = null;

  // Similar option to this. Too lazy to write my own docs.
  // http://www.squid-cache.org/Doc/config/half_closed_clients/
  // https://wiki.squid-cache.org/SquidFaq/InnerWorkings#What_is_a_half-closed_filedescriptor.3F
  httpAllowHalfOpen = false;
  timeout = 0;
  maxHeadersCount: number | null = null;
  maxRequestsPerSocket = 0;
  connectionsCheckingInterval = 30_000;
  requestTimeout: number = 0;
  headersTimeout: number = 0;
  requireHostHeader: boolean = false;
  joinDuplicateHeaders: boolean = false;
  rejectNonStandardBodyWrites: boolean = false;
  keepAliveTimeout: number = 5_000;
  port: number | null = null;

  constructor(options?: ServerOptions, requestListener?: RequestListener) {
    if (!flags.enableNodejsHttpServerModules) {
      throw new ERR_METHOD_NOT_IMPLEMENTED('Server');
    }
    super();

    if (options != null) {
      // @ts-expect-error TS2345 TODO(soon): Find a better way to handle this type mismatch.
      storeHTTPOptions.call(this, options);
    }

    // TODO(soon): Support options.highWaterMark option.

    if (typeof options === 'function') {
      requestListener = options;
      options = {};
    } else if (options == null) {
      options = {};
    } else {
      validateObject(options, 'options');
    }

    if (requestListener) {
      this.on('request', requestListener);
    }

    this[kUniqueHeaders] = parseUniqueHeadersOption(
      options.uniqueHeaders as (string | string[])[]
    );
  }

  close(callback?: VoidFunction): this {
    httpServerPreClose(this);
    if (this.port != null) {
      portMapper.delete(this.port);
      this.port = null;
    }
    this.emit('close');
    queueMicrotask(() => {
      callback?.();
    });
    return this;
  }

  closeAllConnections(): void {
    // It doesn't make sense to support this method.
    // Leave it as a noop.
  }

  closeIdleConnections(): void {
    // It doesn't make sense to support this method.
    // Leave it as a noop.
  }

  setTimeout(
    msecs?: number | ((socket: Socket) => void),
    callback?: (socket: Socket) => void
  ): this {
    if (typeof msecs === 'function') {
      callback = msecs;
      msecs = undefined;
    } else if (typeof msecs === 'number') {
      this.timeout = msecs;
    }

    if (typeof callback === 'function') {
      this.once('timeout', callback);
    }
    return this;
  }

  async #onRequest(request: Request): Promise<Response> {
    const { incoming, response } = this.#toReqRes(request);
    this.emit('connection', this, incoming);
    this.emit('request', incoming, response);
    return getServerResponseFetchResponse(response);
  }

  #toReqRes(request: Request): {
    incoming: IncomingMessage;
    response: ServerResponse;
  } {
    const incoming = new this[kIncomingMessage]();
    const reqUrl = new URL(request.url);
    incoming.url = reqUrl.pathname + reqUrl.search;

    const headers = [];
    for (const [key, value] of request.headers) {
      if (key === 'host') {
        // By default fetch implementation will join "host" header values with a comma.
        // But in order to be node.js compatible, we need to select the first if possible.
        headers.push(key, value.split(', ').at(0) as string);
      } else {
        headers.push(key, value);
      }
    }
    incoming._addHeaderLines(headers, headers.length);

    // TODO(soon): It would be useful if there was a way to expose request.cf properties.
    incoming.method = request.method;
    incoming._stream = request.body;

    const response = new this[kServerResponse](incoming);
    return { incoming, response };
  }

  listen(...args: unknown[]): this {
    const [options, callback] = _normalizeArgs(args);
    if (typeof options.port === 'number' || typeof options.port === 'string') {
      validatePort(options.port, 'options.port');
    }

    if (!('port' in options)) {
      throw new ERR_INVALID_ARG_VALUE(
        'options',
        options,
        'must have the property "port"'
      );
    }

    if (this.port != null || portMapper.has(Number(options.port))) {
      throw new ERR_SERVER_ALREADY_LISTEN();
    }

    if (callback !== null) {
      this.once('listening', callback as (...args: unknown[]) => unknown);
    }

    this.port = this.#findSuitablePort(Number(options.port));
    portMapper.set(this.port, { fetch: this.#onRequest.bind(this) });
    queueMicrotask(() => {
      this.emit('listening');
    });
    return this;
  }

  #findSuitablePort(port: number): number {
    // We don't have to check if portMapper has it because the caller
    // already validates the uniqueness of the port and calls this method.
    if (port !== 0) {
      return port;
    }

    // Let's try at most 10 times to find a suitable port.
    // If we can't find by that time, let's bail and throw an error.
    for (let i = 0; i < 10; i++) {
      port = Math.floor(Math.random() * 65535) + 1;
      if (!portMapper.has(port)) {
        return port;
      }
    }

    // This is unlikely to happen, but just in case.
    throw new Error('Failed to find a suitable port after 10 attempts');
  }

  getConnections(_cb?: (err: Error | null, count: number) => void): this {
    // This method is originally implemented in net.Server.
    // Since we don't implement net.Server yet, we provide this stub implementation for now.
    // TODO(soon): Revisit this once we implement net.Server
    return this;
  }

  ref(): this {
    // It doesn't make sense to implement these at all as they are very specific to the way
    // Node.js' event loop and process model works.
    return this;
  }

  unref(): this {
    // It doesn't make sense to implement these at all as they are very specific to the way
    // Node.js' event loop and process model works.
    return this;
  }

  get listening(): boolean {
    return this.port != null;
  }

  address(): string | AddressInfo | null {
    if (this.port == null) return null;
    return { port: this.port, family: 'IPv4', address: '127.0.0.1' };
  }

  get maxConnections(): number {
    // TODO(soon): Find a correct value for this.
    return Infinity;
  }

  get connections(): number {
    // TODO(soon): Implement this.
    return 0;
  }

  async [Symbol.asyncDispose](): Promise<void> {
    // eslint-disable-next-line @typescript-eslint/no-invalid-void-type
    const { promise, resolve } = Promise.withResolvers<void>();
    this.close(resolve);
    return promise;
  }
}

// We use this handler to not expose this.#fetchResponse to outside world.
let getServerResponseFetchResponse: (
  response: ServerResponse
) => Promise<Response>;

// Data flow:
// 1. Before headers are sent: Data is buffered in a chunks array
//    - MessageBuffer emits '_dataWritten' events with sequential indices (0, 1, 2...)
//    - Each chunk is stored at its index position
// 2. When headers are sent: Create Response with ReadableStream
//    - Flush all buffered chunks to the stream
//    - Clear the array with chunks.length = 0 to free memory
//    - Set up listeners for future data
// 3. After headers: Data streams directly without buffering
//    - New '_dataWritten' events are immediately enqueued to the stream
// 4. Completion: 'finish' event closes the ReadableStream
export class ServerResponse<Req extends IncomingMessage = IncomingMessage>
  extends OutgoingMessage
  implements _ServerResponse
{
  override [kOutHeaders]: Record<string, [string, string | string[]]> | null =
    null;

  statusCode = 200;
  statusMessage = 'unknown';

  #fetchResponse: Promise<Response>;
  #encoder = new TextEncoder();

  static {
    getServerResponseFetchResponse = (
      response: ServerResponse
    ): Promise<Response> => {
      return response.#fetchResponse;
    };
  }

  constructor(req: Req, options: ServerOptions = {}) {
    if (!flags.enableNodejsHttpServerModules) {
      throw new ERR_METHOD_NOT_IMPLEMENTED('ServerResponse');
    }

    super(req, options);

    if (req.httpVersionMajor < 1 || req.httpVersionMinor < 1) {
      this.useChunkedEncodingByDefault = chunkExpression.test(
        (req.headers.te as string | undefined) ?? ''
      );
      this.shouldKeepAlive = false;
    }

    const { promise, resolve, reject } = Promise.withResolvers<Response>();

    let streamController: StreamController | null = null;
    const chunks: (Buffer | Uint8Array)[] = [];
    const state: { bytesWritten: number; contentLength: number | null } = {
      bytesWritten: 0,
      contentLength: null,
    };

    const handleData = (event: DataWrittenEvent): void => {
      const chunk = this.#dataFromDataWrittenEvent(event);
      state.bytesWritten += chunk.length;

      if (
        state.contentLength !== null &&
        state.bytesWritten > state.contentLength
      ) {
        const error = new Error(
          `Content-Length mismatch: wrote ${state.bytesWritten} bytes but Content-Length header was ${state.contentLength}`
        );
        streamController?.error(error);
        return;
      }

      if (streamController) {
        streamController.enqueue(chunk);
      } else {
        chunks[event.index] = chunk;
      }
    };

    this.on('_dataWritten', handleData);
    this.once('error', reject);

    this.once(
      '_headersSent',
      ({ statusCode, statusMessage, headers }: HeadersSentEvent) => {
        for (const [name, value] of headers) {
          // Optimization: Avoid unnecessary string comparison by checking length first
          if (name.length === 14 && name.toLowerCase() === 'content-length') {
            state.contentLength = parseInt(value, 10);
            break;
          }
        }

        resolve(
          this.#toFetchResponse({
            statusCode,
            statusText: statusMessage,
            sentHeaders: headers,
            onStreamStart: (controller) => {
              streamController = controller;
              for (const chunk of chunks) {
                controller.enqueue(chunk);
              }
              chunks.length = 0;
            },
            state,
          })
        );
      }
    );

    this.#fetchResponse = promise;
  }

  #toFetchResponse({
    statusCode,
    statusText,
    sentHeaders,
    onStreamStart,
    state,
  }: {
    statusCode: number;
    statusText: string;
    sentHeaders: [header: string, value: string][];
    onStreamStart: (controller: StreamController) => void;
    state: { bytesWritten: number; contentLength: number | null };
  }): Response {
    const headers = new Headers(sentHeaders);
    let body = null;

    if (this._hasBody) {
      body = new ReadableStream<Uint8Array>({
        start: (controller): void => {
          onStreamStart(controller);
          this.once('finish', () => {
            if (
              state.contentLength !== null &&
              state.bytesWritten !== state.contentLength
            ) {
              controller.error(
                new Error(
                  `Content-Length mismatch: wrote ${state.bytesWritten} bytes but Content-Length header was ${state.contentLength}`
                )
              );
            } else {
              controller.close();
            }
          });
          this.on('error', (error) => {
            controller.error(error);
          });
        },
        cancel: (reason: unknown): void => {
          this.destroy(reason);
        },
      });
    }

    return new Response(body, {
      status: statusCode,
      statusText,
      headers,
    });
  }

  #dataFromDataWrittenEvent({
    index,
    entry: { data, encoding },
  }: DataWrittenEvent): Buffer | Uint8Array {
    if (typeof data === 'string') {
      // First chunk includes headers - skip them
      if (index === 0) {
        data = data.slice(this.writtenHeaderBytes);
      }

      return encoding == null || encoding === 'utf8' || encoding === 'utf-8'
        ? this.#encoder.encode(data)
        : Buffer.from(data, encoding);
    }

    return data ?? Buffer.alloc(0);
  }

  assignSocket(_socket: Socket): void {
    // We don't plan to support this method, since our
    // implementation is based on fetch and not Node.js sockets.
    throw new ERR_METHOD_NOT_IMPLEMENTED('assignSocket');
  }

  detachSocket(_socket: Socket): void {
    // We don't plan to support this method, since our
    // implementation is based on fetch and not Node.js sockets.
    throw new ERR_METHOD_NOT_IMPLEMENTED('detachSocket');
  }

  writeContinue(_cb: VoidFunction): void {
    // There is no path forward to support this with fetch or kj.
    throw new ERR_METHOD_NOT_IMPLEMENTED('writeContinue');
  }

  // Ref: https://github.com/DefinitelyTyped/DefinitelyTyped/pull/73275
  // @ts-expect-error TS2416 This is a bug with @types/node.
  writeProcessing(_cb: VoidFunction): void {
    // There is no path forward to support this with fetch or kj.
    throw new ERR_METHOD_NOT_IMPLEMENTED('writeProcessing');
  }

  writeEarlyHints(_hints: unknown, _cb: VoidFunction): void {
    // There is no path forward to support this with fetch or kj.
    throw new ERR_METHOD_NOT_IMPLEMENTED('writeEarlyHints');
  }

  override _implicitHeader(): void {
    this.writeHead(this.statusCode);
  }

  writeHead(
    statusCode: number,
    reason?: string | OutgoingHttpHeaders | OutgoingHttpHeader[],
    obj?: OutgoingHttpHeaders | OutgoingHttpHeader[]
  ): this {
    if (this._header) {
      throw new ERR_HTTP_HEADERS_SENT('write');
    }

    const originalStatusCode = statusCode;

    statusCode |= 0;
    if (statusCode < 200 || statusCode > 999) {
      // < 100 status codes are not supported by cloudflare workers.
      throw new ERR_HTTP_INVALID_STATUS_CODE(originalStatusCode);
    }

    if (typeof reason === 'string') {
      // writeHead(statusCode, reasonPhrase[, headers])
      this.statusMessage = reason;
    } else {
      // writeHead(statusCode[, headers])
      this.statusMessage ||= STATUS_CODES[`${statusCode}`] || 'unknown';
      obj ??= reason;
    }
    this.statusCode = statusCode;

    let headers;
    if (this[kOutHeaders]) {
      // Slow-case: when progressive API and header fields are passed.
      let k;
      if (Array.isArray(obj)) {
        if (obj.length % 2 !== 0) {
          throw new ERR_INVALID_ARG_VALUE('headers', obj);
        }

        // Headers in obj should override previous headers but still
        // allow explicit duplicates. To do so, we first remove any
        // existing conflicts, then use appendHeader.

        for (let n = 0; n < obj.length; n += 2) {
          k = obj[n + 0];
          this.removeHeader(String(k));
        }

        for (let n = 0; n < obj.length; n += 2) {
          k = obj[n];
          if (k) {
            this.appendHeader(`${k}`, obj[n + 1] as OutgoingHttpHeader);
          }
        }
      } else if (obj) {
        const keys = Object.keys(obj);
        // Retain for(;;) loop for performance reasons
        // Refs: https://github.com/nodejs/node/pull/30958
        for (let i = 0; i < keys.length; i++) {
          k = keys[i];
          if (k) {
            this.setHeader(k, obj[k] as OutgoingHttpHeader);
          }
        }
      }
      // Only progressive api is used
      headers = this[kOutHeaders];
    } else {
      // Only writeHead() called
      headers = obj;
    }

    if (_checkInvalidHeaderChar(this.statusMessage)) {
      throw new ERR_INVALID_CHAR('statusMessage');
    }

    const statusLine = `HTTP/1.1 ${statusCode} ${this.statusMessage}\r\n`;

    if (
      statusCode === 204 ||
      statusCode === 304 ||
      (statusCode >= 100 && statusCode <= 199)
    ) {
      // RFC 2616, 10.2.5:
      // The 204 response MUST NOT include a message-body, and thus is always
      // terminated by the first empty line after the header fields.
      // RFC 2616, 10.3.5:
      // The 304 response MUST NOT contain a message-body, and thus is always
      // terminated by the first empty line after the header fields.
      // RFC 2616, 10.1 Informational 1xx:
      // This class of status code indicates a provisional response,
      // consisting only of the Status-Line and optional headers, and is
      // terminated by an empty line.
      this._hasBody = false;
    }

    // Convert headers to a compatible type for _storeHeader
    const convertedHeaders =
      headers && !Array.isArray(headers)
        ? (headers as OutgoingHttpHeaders)
        : headers;
    this._storeHeader(statusLine, convertedHeaders ?? null);

    return this;
  }

  writeHeader = this.writeHead.bind(this);
}

export function setupConnectionsTracking(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('setupConnectionsTracking');
}

export interface BaseWithHttpOptions<
  IM extends IncomingMessage = IncomingMessage,
  SR extends ServerResponse = ServerResponse,
> {
  [kIncomingMessage]: IM;
  [kServerResponse]: SR;
  requestTimeout: number;
  headersTimeout: number;
  requireHostHeader: boolean;
  joinDuplicateHeaders: boolean;
  rejectNonStandardBodyWrites: boolean;
}

export function storeHTTPOptions(
  this: BaseWithHttpOptions,
  options: ServerOptions
): void {
  // @ts-expect-error TS2322 Type mismatch.
  this[kIncomingMessage] = options.IncomingMessage || IncomingMessage;
  // @ts-expect-error TS2322 Type mismatch.
  this[kServerResponse] = options.ServerResponse || ServerResponse;

  const maxHeaderSize = options.maxHeaderSize;
  if (maxHeaderSize !== undefined) {
    validateInteger(maxHeaderSize, 'maxHeaderSize', 0);
    throw new ERR_OPTION_NOT_IMPLEMENTED('maxHeaderSize');
  }

  const insecureHTTPParser = options.insecureHTTPParser;
  if (insecureHTTPParser !== undefined) {
    validateBoolean(insecureHTTPParser, 'options.insecureHTTPParser');
    throw new ERR_OPTION_NOT_IMPLEMENTED('insecureHTTPParser');
  }

  const requestTimeout = options.requestTimeout;
  if (requestTimeout !== undefined) {
    validateInteger(requestTimeout, 'requestTimeout', 0);
    this.requestTimeout = requestTimeout;
  } else {
    this.requestTimeout = 300_000; // 5 minutes
  }

  const headersTimeout = options.headersTimeout;
  if (headersTimeout !== undefined) {
    validateInteger(headersTimeout, 'headersTimeout', 0);
    this.headersTimeout = headersTimeout;
  } else {
    this.headersTimeout = Math.min(60_000, this.requestTimeout); // Minimum between 60 seconds or requestTimeout
  }

  if (
    this.requestTimeout > 0 &&
    this.headersTimeout > 0 &&
    this.headersTimeout > this.requestTimeout
  ) {
    throw new ERR_OUT_OF_RANGE(
      'headersTimeout',
      '<= requestTimeout',
      headersTimeout
    );
  }

  const keepAliveTimeout = options.keepAliveTimeout;
  if (keepAliveTimeout !== undefined) {
    validateInteger(keepAliveTimeout, 'keepAliveTimeout', 0);
    throw new ERR_OPTION_NOT_IMPLEMENTED('keepAliveTimeout');
  }

  const connectionsCheckingInterval = options.connectionsCheckingInterval;
  if (connectionsCheckingInterval !== undefined) {
    validateInteger(
      connectionsCheckingInterval,
      'connectionsCheckingInterval',
      0
    );
    throw new ERR_OPTION_NOT_IMPLEMENTED('connectionsCheckingInterval');
  }

  const requireHostHeader = options.requireHostHeader;
  if (requireHostHeader !== undefined) {
    validateBoolean(requireHostHeader, 'options.requireHostHeader');
    this.requireHostHeader = requireHostHeader;
  } else {
    this.requireHostHeader = true;
  }

  const joinDuplicateHeaders = options.joinDuplicateHeaders;
  if (joinDuplicateHeaders !== undefined) {
    validateBoolean(joinDuplicateHeaders, 'options.joinDuplicateHeaders');
  }
  this.joinDuplicateHeaders = joinDuplicateHeaders ?? false;

  const rejectNonStandardBodyWrites = options.rejectNonStandardBodyWrites;
  if (rejectNonStandardBodyWrites !== undefined) {
    validateBoolean(
      rejectNonStandardBodyWrites,
      'options.rejectNonStandardBodyWrites'
    );
    this.rejectNonStandardBodyWrites = rejectNonStandardBodyWrites;
  } else {
    this.rejectNonStandardBodyWrites = false;
  }
}

export function _connectionListener(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('_connectionListener');
}

export function httpServerPreClose(server: Server): void {
  server.closeIdleConnections();
  clearInterval(server[kConnectionsCheckingInterval]);
}
