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
import { getDefaultHighWaterMark } from 'node-internal:streams_state';
import {
  kUniqueHeaders,
  OutgoingMessage,
  parseUniqueHeadersOption,
} from 'node-internal:internal_http_outgoing';
import {
  validateBoolean,
  validateFunction,
  validateInteger,
  validateObject,
  validatePort,
  validateNumber,
} from 'node-internal:validators';
import { portMapper } from 'cloudflare-internal:http';
import {
  IncomingMessage,
  setIncomingMessageSocket,
  setIncomingRequestBody,
} from 'node-internal:internal_http_incoming';
import { STATUS_CODES } from 'node-internal:internal_http_constants';
import {
  kServerResponse,
  kIncomingMessage,
  splitHeaderValue,
} from 'node-internal:internal_http_util';
import {
  kOutHeaders,
  type WrittenDataBufferEntry,
  type HeadersSentEvent,
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

const enableNodejsHttpServerModules =
  !!Cloudflare.compatibilityFlags['enable_nodejs_http_server_modules'];

export const kConnectionsCheckingInterval = Symbol(
  'http.server.connectionsCheckingInterval'
);

export type DataWrittenEvent = {
  index: number;
  entry: WrittenDataBufferEntry;
};

// By default Node.js forbids the following headers to be joined by comma.
// Cloudflare workers implementation of Server, uses Fetch and by default
// fetch joins them. Therefore, we need to maintain this list of headers
// to filter and only return the first match to be Node.js compatible.
//
// For more reference, here is a Node.js test that validates this behavior:
// https://github.com/nodejs/node/blob/af77e4bf2f8bee0bc23f6ee129d6ca97511d34b9/test/parallel/test-http-server-multiheaders2.js
const multipleForbiddenHeaders = [
  'host',
  'content-type',
  'user-agent',
  'referer',
  'authorization',
  'proxy-authorization',
  'if-modified-since',
  'if-unmodified-since',
  'from',
  'location',
  'max-forwards',
];

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
  requestTimeout: number = 300_000;
  headersTimeout: number = 60_000;
  requireHostHeader: boolean = false;
  joinDuplicateHeaders: boolean = false;
  rejectNonStandardBodyWrites: boolean = false;
  keepAliveTimeout: number = 5_000;
  keepAliveTimeoutBuffer: number = 1_000;
  highWaterMark: number = getDefaultHighWaterMark();
  #port: number | null = null;

  constructor(options?: ServerOptions, requestListener?: RequestListener) {
    if (!enableNodejsHttpServerModules) {
      throw new ERR_METHOD_NOT_IMPLEMENTED('Server');
    }
    super();

    if (options != null) {
      // @ts-expect-error TS2345 TODO(soon): Find a better way to handle this type mismatch.
      storeHTTPOptions.call(this, options);
    }

    if (options?.highWaterMark !== undefined) {
      validateNumber(options.highWaterMark, 'options.highWaterMark');
      if (options.highWaterMark > 0) {
        this.highWaterMark = options.highWaterMark;
      }
    }

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

  // Failing to call close() on a http server may result in the server being leaked.
  // To prevent this, call close() when you're done with the server, or use
  // explicit resource management. (example: await using s = createServer())
  close(callback?: VoidFunction): this {
    httpServerPreClose(this);
    if (this.#port != null) {
      portMapper.delete(this.#port);
      this.#port = null;
    }
    if (typeof callback === 'function') {
      this.once('close', callback);
    }
    queueMicrotask(() => {
      this.emit('close');
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

  async #onRequest(
    request: Request,
    env: unknown,
    ctx: unknown
  ): Promise<Response> {
    const { incoming, response } = this.#toReqRes(request, env, ctx);
    try {
      this.emit('connection', this, incoming);
      this.emit('request', incoming, response);
      return await getServerResponseFetchResponse(response);
    } catch (error: unknown) {
      response.destroy(error);
      throw error;
    }
  }

  #toReqRes(
    request: Request,
    env: unknown,
    ctx: unknown
  ): {
    incoming: IncomingMessage;
    response: ServerResponse;
  } {
    const incoming = new this[kIncomingMessage]();
    setIncomingMessageSocket(incoming, {
      headers: request.headers,
      localPort: this.#port as number,
    });
    const reqUrl = new URL(request.url);
    incoming.url = reqUrl.pathname + reqUrl.search;

    const headers = [];
    for (const [key, value] of request.headers) {
      if (multipleForbiddenHeaders.includes(key)) {
        // By default fetch implementation will join the following header values with a comma.
        // But in order to be node.js compatible, we need to select the first if possible.
        // Use RFC 7230 compliant splitting that respects quoted-string constructions.
        headers.push(key, splitHeaderValue(value));
      } else {
        headers.push(key, value);
      }
    }
    incoming._addHeaderLines(headers, headers.length);

    incoming.method = request.method;
    setIncomingRequestBody(incoming, request.body);

    // We provide a way for users to access to the Cloudflare-specific
    // request properties, such as `cf` for accessing Cloudflare-specific request metadata.
    incoming.cloudflare = {
      env,
      ctx,
    };
    if ('cf' in request) {
      incoming.cloudflare.cf = request.cf as Record<string, unknown>;
    }

    const response = new this[kServerResponse](incoming, {
      highWaterMark: this.highWaterMark,
    });
    return { incoming, response };
  }

  // We only support the listen() variant where a port number is passed or left
  // unspecified. Such cases are listen(), listen(0, () => {}), listen(() => {}) etc.
  listen(...args: unknown[]): this {
    const [options, callback] = _normalizeArgs(args);
    let port: number | undefined;
    if (typeof options.port === 'number' || typeof options.port === 'string') {
      port = validatePort(options.port, 'options.port');
    }

    // If port number is not provided, default to 0, just like Node.js
    if (port == null) {
      port = 0;
    }

    if (this.#port != null || portMapper.has(port)) {
      throw new ERR_SERVER_ALREADY_LISTEN();
    }

    if (callback !== null) {
      this.once('listening', callback as (...args: unknown[]) => unknown);
    }

    this.#port = this.#findSuitablePort(port);
    // @ts-expect-error TS2322 Type mismatch. Not needed.
    portMapper.set(this.#port, { fetch: this.#onRequest.bind(this) });
    queueMicrotask(() => {
      // If any of the listening handlers (here and in any of the other queueMicrotask(...) instances here,
      // if the listening handlers throw an error, that will end up being reported to
      // reportError(...) and will cause the globalThis error event to be triggered.
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

  getConnections(callback?: (err: Error | null, count: number) => void): this {
    if (callback) {
      validateFunction(callback, 'callback');
      queueMicrotask(() => {
        callback(null, 0);
      });
    }
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
    return this.#port != null;
  }

  // We always return 127.0.0.1 as the address().address.
  address(): string | AddressInfo | null {
    if (this.#port == null) return null;
    return { port: this.#port, family: 'IPv4', address: '127.0.0.1' };
  }

  get maxConnections(): number {
    return Infinity;
  }

  get connections(): number {
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
// @ts-expect-error TS2720 Trailers related methods/attributes are missing.
export class ServerResponse<Req extends IncomingMessage = IncomingMessage>
  extends OutgoingMessage
  // @ts-expect-error TS2720 `rawTrailers` attribute is missing.
  implements _ServerResponse<Req>
{
  override [kOutHeaders]: Record<string, [string, string | string[]]> | null =
    null;

  statusCode = 200;
  statusMessage = 'unknown';

  #fetchResponse: Promise<Response>;

  static {
    getServerResponseFetchResponse = (
      response: ServerResponse
    ): Promise<Response> => {
      return response.#fetchResponse;
    };
  }

  constructor(req: Req, options: ServerOptions = {}) {
    if (!enableNodejsHttpServerModules) {
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

    let streamController: ReadableStreamController<Uint8Array> | null = null;
    const chunks: (Buffer | Uint8Array)[] = [];
    const state: { bytesWritten: number; contentLength: number | null } = {
      bytesWritten: 0,
      contentLength: null,
    };

    const handleData = (events: DataWrittenEvent[]): void => {
      for (const event of events) {
        let chunk = this.#dataFromDataWrittenEvent(event);

        // Trim chunk if it would exceed content-length
        if (
          state.contentLength !== null &&
          state.bytesWritten + chunk.length > state.contentLength
        ) {
          const remainingBytes = state.contentLength - state.bytesWritten;
          if (remainingBytes > 0) {
            chunk = chunk.slice(0, remainingBytes);
          } else {
            continue; // Skip this chunk entirely
          }
        }

        state.bytesWritten += chunk.length;

        if (streamController) {
          if (chunk.length > 0) {
            // @ts-expect-error TS2345 Buffer extends Uint8Array, but has ArrayBufferLike instead of ArrayBuffer.
            streamController.enqueue(chunk);
          }
        } else {
          chunks[event.index] = chunk;
        }
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
            headers,
            onStreamStart: (controller) => {
              streamController = controller;
              for (const chunk of chunks) {
                // @ts-expect-error TS2345 Buffer extends Uint8Array, but has ArrayBufferLike instead of ArrayBuffer.
                controller.enqueue(chunk);
              }
              chunks.length = 0;
            },
          })
        );

        this._closed = true;
        this.emit('close');
      }
    );

    this.#fetchResponse = promise;
  }

  #toFetchResponse({
    statusCode,
    statusText,
    headers,
    onStreamStart,
  }: {
    statusCode: number;
    statusText: string;
    headers: Headers;
    onStreamStart: (controller: ReadableStreamController<Uint8Array>) => void;
  }): Response {
    let body = null;

    if (this._hasBody) {
      body = new ReadableStream<Uint8Array>({
        type: 'bytes',
        start: (controller): void => {
          onStreamStart(controller);
          this.once('finish', () => {
            controller.close();
          });
          this.on('error', controller.error.bind(controller));
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

      return Buffer.from(data, encoding ?? undefined);
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
      if (Array.isArray(obj)) {
        if (obj.length % 2 !== 0) {
          throw new ERR_INVALID_ARG_VALUE('headers', obj);
        }

        // Headers in obj should override previous headers but still
        // allow explicit duplicates. To do so, we first remove any
        // existing conflicts, then use appendHeader.

        for (let n = 0; n < obj.length; n += 2) {
          this.removeHeader(`${obj[n]}`);
        }

        for (let n = 0; n < obj.length; n += 2) {
          this.appendHeader(`${obj[n]}`, obj[n + 1] as OutgoingHttpHeader);
        }
      } else if (obj) {
        for (const key of Object.keys(obj)) {
          if (obj[key]) {
            this.setHeader(key, obj[key]);
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

    // TODO(soon): Unnecessary additional complexity when we could just build
    // the Headers object up directly and skip the additional string wrangling.
    const statusLine = `HTTP/1.1 ${statusCode} ${this.statusMessage}\r\n`;

    if (statusCode === 204 || statusCode === 304) {
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
    // If enabled it will use a HTTP parser with leniency flags enabled.
    // Since our implementation does not use any http parser, and uses "fetch" API,
    // it doesn't make sense to support this option.
    validateBoolean(insecureHTTPParser, 'options.insecureHTTPParser');
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

  const keepAliveTimeoutBuffer = options.keepAliveTimeoutBuffer;
  if (keepAliveTimeoutBuffer !== undefined) {
    validateInteger(keepAliveTimeoutBuffer, 'keepAliveTimeoutBuffer');
    throw new ERR_OPTION_NOT_IMPLEMENTED('keepAliveTimeoutBuffer');
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
