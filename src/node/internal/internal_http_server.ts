// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  ERR_METHOD_NOT_IMPLEMENTED,
  ERR_HTTP_HEADERS_SENT,
  ERR_HTTP_INVALID_STATUS_CODE,
  ERR_INVALID_CHAR,
  ERR_INVALID_ARG_VALUE,
  ERR_OUT_OF_RANGE,
  ERR_OPTION_NOT_IMPLEMENTED,
  ERR_INVALID_ARG_TYPE,
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
import type { Socket } from 'node:net';

export const kServerResponse = Symbol('ServerResponse');
export const kConnectionsCheckingInterval = Symbol(
  'http.server.connectionsCheckingInterval'
);

export const STATUS_CODES = {
  100: 'Continue', // RFC 7231 6.2.1
  101: 'Switching Protocols', // RFC 7231 6.2.2
  102: 'Processing', // RFC 2518 10.1 (obsoleted by RFC 4918)
  103: 'Early Hints', // RFC 8297 2
  200: 'OK', // RFC 7231 6.3.1
  201: 'Created', // RFC 7231 6.3.2
  202: 'Accepted', // RFC 7231 6.3.3
  203: 'Non-Authoritative Information', // RFC 7231 6.3.4
  204: 'No Content', // RFC 7231 6.3.5
  205: 'Reset Content', // RFC 7231 6.3.6
  206: 'Partial Content', // RFC 7233 4.1
  207: 'Multi-Status', // RFC 4918 11.1
  208: 'Already Reported', // RFC 5842 7.1
  226: 'IM Used', // RFC 3229 10.4.1
  300: 'Multiple Choices', // RFC 7231 6.4.1
  301: 'Moved Permanently', // RFC 7231 6.4.2
  302: 'Found', // RFC 7231 6.4.3
  303: 'See Other', // RFC 7231 6.4.4
  304: 'Not Modified', // RFC 7232 4.1
  305: 'Use Proxy', // RFC 7231 6.4.5
  307: 'Temporary Redirect', // RFC 7231 6.4.7
  308: 'Permanent Redirect', // RFC 7238 3
  400: 'Bad Request', // RFC 7231 6.5.1
  401: 'Unauthorized', // RFC 7235 3.1
  402: 'Payment Required', // RFC 7231 6.5.2
  403: 'Forbidden', // RFC 7231 6.5.3
  404: 'Not Found', // RFC 7231 6.5.4
  405: 'Method Not Allowed', // RFC 7231 6.5.5
  406: 'Not Acceptable', // RFC 7231 6.5.6
  407: 'Proxy Authentication Required', // RFC 7235 3.2
  408: 'Request Timeout', // RFC 7231 6.5.7
  409: 'Conflict', // RFC 7231 6.5.8
  410: 'Gone', // RFC 7231 6.5.9
  411: 'Length Required', // RFC 7231 6.5.10
  412: 'Precondition Failed', // RFC 7232 4.2
  413: 'Payload Too Large', // RFC 7231 6.5.11
  414: 'URI Too Long', // RFC 7231 6.5.12
  415: 'Unsupported Media Type', // RFC 7231 6.5.13
  416: 'Range Not Satisfiable', // RFC 7233 4.4
  417: 'Expectation Failed', // RFC 7231 6.5.14
  418: "I'm a Teapot", // RFC 7168 2.3.3
  421: 'Misdirected Request', // RFC 7540 9.1.2
  422: 'Unprocessable Entity', // RFC 4918 11.2
  423: 'Locked', // RFC 4918 11.3
  424: 'Failed Dependency', // RFC 4918 11.4
  425: 'Too Early', // RFC 8470 5.2
  426: 'Upgrade Required', // RFC 2817 and RFC 7231 6.5.15
  428: 'Precondition Required', // RFC 6585 3
  429: 'Too Many Requests', // RFC 6585 4
  431: 'Request Header Fields Too Large', // RFC 6585 5
  451: 'Unavailable For Legal Reasons', // RFC 7725 3
  500: 'Internal Server Error', // RFC 7231 6.6.1
  501: 'Not Implemented', // RFC 7231 6.6.2
  502: 'Bad Gateway', // RFC 7231 6.6.3
  503: 'Service Unavailable', // RFC 7231 6.6.4
  504: 'Gateway Timeout', // RFC 7231 6.6.5
  505: 'HTTP Version Not Supported', // RFC 7231 6.6.6
  506: 'Variant Also Negotiates', // RFC 2295 8.1
  507: 'Insufficient Storage', // RFC 4918 11.5
  508: 'Loop Detected', // RFC 5842 7.2
  509: 'Bandwidth Limit Exceeded',
  510: 'Not Extended', // RFC 2774 7
  511: 'Network Authentication Required', // RFC 6585 6
};

export type DataWrittenEvent = {
  index: number;
  entry: WrittenDataBufferEntry;
};

// @ts-expect-error TS2720 net.Server inconsistencies.
export class Server
  extends EventEmitter
  implements _Server, BaseWithHttpOptions
{
  [kConnectionsCheckingInterval]?: number;
  [kUniqueHeaders]: Set<string> | null = null;

  // Similar option to this. Too lazy to write my own docs.
  // http://www.squid-cache.org/Doc/config/half_closed_clients/
  // https://wiki.squid-cache.org/SquidFaq/InnerWorkings#What_is_a_half-closed_filedescriptor.3F
  httpAllowHalfOpen = false;
  timeout = 0;
  maxHeadersCount: number | null = null;
  maxRequestsPerSocket = 0;

  requestTimeout: number = 0;
  headersTimeout: number = 0;
  requireHostHeader: boolean = false;
  joinDuplicateHeaders: boolean = false;
  rejectNonStandardBodyWrites: boolean = false;
  keepAliveTimeout: number = 5_000;
  port?: number;

  constructor(options?: ServerOptions, requestListener?: RequestListener) {
    super();

    if (options != null) {
      storeHTTPOptions.call(this, options);
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

    this.on('listening', setupConnectionsTracking);

    this[kUniqueHeaders] = parseUniqueHeadersOption(
      options.uniqueHeaders as (string | string[])[]
    );
  }

  close(): this {
    httpServerPreClose(this);
    if (this.port) {
      portMapper.delete(this.port);
    }
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
    const incoming = new IncomingMessage();
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

    incoming.method = request.method;
    incoming._stream = request.body;

    const response = new ServerResponse(incoming);
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

    if (this.port != null) {
      throw new ERR_SERVER_ALREADY_LISTEN();
    }

    if (portMapper.has(Number(options.port))) {
      throw new Error(`Port ${options.port} is already in use`);
    }

    if (callback !== null) {
      this.once('listening', callback as (...args: unknown[]) => unknown);
    }

    this.port = Number(options.port);
    portMapper.set(this.port, { fetch: this.#onRequest.bind(this) });
    queueMicrotask(() => {
      callback?.();
    });
    return this;
  }

  getConnections(_cb?: (err: Error | null, count: number) => void): this {
    // This method is originally implemented in net.Server.
    // Since we don't implement net.Server yet, we provide this stub implementation for now.
    // TODO(soon): Revisit this once we implement net.Server
    return this;
  }

  ref(): this {
    // This method is originally implemented in net.Server.
    // Since we don't implement net.Server yet, we provide this stub implementation for now.
    // TODO(soon): Revisit this once we implement net.Server
    return this;
  }

  unref(): this {
    // This method is originally implemented in net.Server.
    // Since we don't implement net.Server yet, we provide this stub implementation for now.
    // TODO(soon): Revisit this once we implement net.Server
    return this;
  }
}

// We use this handler to not expose this.#fetchResponse to outside world.
let getServerResponseFetchResponse: (
  response: ServerResponse
) => Promise<Response>;

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
    super(req, options);

    if (req.httpVersionMajor < 1 || req.httpVersionMinor < 1) {
      this.useChunkedEncodingByDefault = chunkExpression.test(
        (req.headers.te as string | undefined) ?? ''
      );
      this.shouldKeepAlive = false;
    }

    const { promise, resolve } = Promise.withResolvers<Response>();

    let finished = false;
    this.once('finish', () => (finished = true));
    const chunks: (Buffer | Uint8Array)[] = [];
    const handler = (event: DataWrittenEvent): void => {
      if (finished) return;
      chunks[event.index] = this.#dataFromDataWrittenEvent(event);
    };
    this.on('_dataWritten', handler);
    this.on(
      '_headersSent',
      ({ statusCode, statusMessage, headers }: HeadersSentEvent) => {
        this.off('_dataWritten', handler);
        resolve(
          this.#toFetchResponse({
            statusCode,
            statusText: statusMessage,
            sentHeaders: headers,
            chunks,
            finished,
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
    chunks,
    finished,
  }: {
    statusCode: number;
    statusText: string;
    sentHeaders: [header: string, value: string][];
    chunks: (Buffer | Uint8Array)[];
    finished: boolean;
  }): Response {
    const headers = new Headers();
    for (const [header, value] of sentHeaders) {
      headers.append(header, value);
    }

    let body = null;

    if (this._hasBody) {
      const _this = this; // eslint-disable-line @typescript-eslint/no-this-alias
      body = new ReadableStream<Uint8Array>({
        start(controller): void {
          for (const chunk of chunks) {
            controller.enqueue(chunk);
          }

          if (finished) {
            controller.close();
          } else {
            _this.on('finish', () => {
              finished = true;
              controller.close();
            });
            _this.on('_dataWritten', (e: DataWrittenEvent) => {
              if (finished) {
                return;
              }
              controller.enqueue(_this.#dataFromDataWrittenEvent(e));
            });
          }
        },
      });
    }

    if (body != null) {
      const contentLength = parseInt(headers.get('content-length') ?? '', 10); // will be NaN if not set

      if (contentLength >= 0) {
        // @ts-expect-error TS2304 Fix this once global types are correct.
        // eslint-disable-next-line @typescript-eslint/no-unsafe-argument,@typescript-eslint/no-unsafe-call
        body = body.pipeThrough(new FixedLengthStream(contentLength));
      }
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
    if (index === 0) {
      if (typeof data !== 'string') {
        console.error('First chunk should be string, not sure what happened.');
        throw new ERR_INVALID_ARG_TYPE(
          'packet.data',
          ['string', 'Buffer', 'Uint8Array'],
          data
        );
      }
      // The first X bytes are header material, so we remove it.
      data = data.slice(this.writtenHeaderBytes);
    }

    if (typeof data === 'string') {
      if (
        encoding === undefined ||
        encoding === 'utf8' ||
        encoding === 'utf-8'
      ) {
        data = this.#encoder.encode(data);
      } else {
        data = Buffer.from(data, encoding ?? undefined);
      }
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
    if (statusCode < 100 || statusCode > 999) {
      throw new ERR_HTTP_INVALID_STATUS_CODE(originalStatusCode);
    }

    if (typeof reason === 'string') {
      // writeHead(statusCode, reasonPhrase[, headers])
      this.statusMessage = reason;
    } else {
      // writeHead(statusCode[, headers])
      this.statusMessage ||=
        STATUS_CODES[statusCode as keyof typeof STATUS_CODES] || 'unknown';
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

export interface BaseWithHttpOptions {
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
