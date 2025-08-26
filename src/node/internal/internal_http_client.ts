// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { _checkIsHttpToken as checkIsHttpToken } from 'node-internal:internal_http';
import {
  kOutHeaders,
  kUniqueHeaders,
  parseUniqueHeadersOption,
} from 'node-internal:internal_http_outgoing';
import { Buffer } from 'node-internal:internal_buffer';
import { urlToHttpOptions, isURL } from 'node-internal:internal_url';
import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_HTTP_TOKEN,
  ERR_OPTION_NOT_IMPLEMENTED,
  ERR_UNESCAPED_CHARACTERS,
  ERR_INVALID_PROTOCOL,
  ERR_INVALID_ARG_VALUE,
  ERR_HTTP_HEADERS_SENT,
} from 'node-internal:internal_errors';
import {
  validateInteger,
  validateBoolean,
  validateFunction,
  validateString,
  validateNumber,
} from 'node-internal:validators';
import { getTimerDuration } from 'node-internal:internal_net';
import { addAbortSignal } from 'node-internal:streams_util';
import { Writable } from 'node-internal:streams_writable';
import type {
  ClientRequest as _ClientRequest,
  RequestOptions,
  OutgoingHttpHeaders,
} from 'node:http';
import {
  IncomingMessage,
  setIncomingMessageFetchResponse,
} from 'node-internal:internal_http_incoming';
import { OutgoingMessage } from 'node-internal:internal_http_outgoing';
import { Agent, globalAgent } from 'node-internal:internal_http_agent';
import type { IncomingMessageCallback } from 'node-internal:internal_http_util';

const INVALID_PATH_REGEX = /[^\u0021-\u00ff]/;

function validateHost(host: unknown, name: string): string {
  if (host != null && typeof host !== 'string') {
    throw new ERR_INVALID_ARG_TYPE(
      `options.${name}`,
      ['string', 'undefined', 'null'],
      host
    );
  }
  return host as string;
}

export class ClientRequest extends OutgoingMessage implements _ClientRequest {
  #abortController = new AbortController();
  #body: Buffer[] = [];
  #incomingMessage?: IncomingMessage;
  #timer: number | null = null;

  _ended: boolean = false;

  timeout?: number;
  method: string = 'GET';
  path: string;
  host: string;
  protocol: string = 'http:';
  port: string = '80';
  joinDuplicateHeaders: boolean | undefined;
  agent: Agent | undefined;

  [kUniqueHeaders]: Set<string> | null = null;

  constructor(
    input: string | URL | RequestOptions | null,
    options?: RequestOptions | IncomingMessageCallback,
    cb?: IncomingMessageCallback
  ) {
    super();

    if (typeof input === 'string') {
      input = urlToHttpOptions(new URL(input));
    } else if (isURL(input)) {
      // url.URL instance
      input = urlToHttpOptions(input);
    } else {
      cb = options as IncomingMessageCallback;
      options = input as RequestOptions;
      input = null;
    }

    if (typeof options === 'function') {
      cb = options;
      options = input ?? {};
    } else {
      options = Object.assign(input ?? {}, options);
    }

    if (options.path) {
      if (INVALID_PATH_REGEX.test(options.path)) {
        throw new ERR_UNESCAPED_CHARACTERS('Request path');
      }
    }

    type AgentLike = Agent | boolean | null | undefined;
    let agent = options.agent as unknown as AgentLike;
    // TODO(soon): Rather than using RequestOptions use our own type that includes our own Agent class type.
    const defaultAgent =
      (options._defaultAgent as unknown as AgentLike) || globalAgent;
    if (agent === false) {
      // @ts-expect-error TS2351 This expression is not constructable.
      // eslint-disable-next-line @typescript-eslint/no-unsafe-assignment,@typescript-eslint/no-unsafe-call
      agent = new defaultAgent.constructor();
    } else if (agent == null) {
      if (typeof options.createConnection !== 'function') {
        agent = defaultAgent as Agent;
      }
    } else if (
      typeof agent === 'object' &&
      typeof agent.addRequest !== 'function'
    ) {
      throw new ERR_INVALID_ARG_TYPE(
        'options.agent',
        ['Agent-like Object', 'undefined', 'false'],
        agent
      );
    }
    this.agent = agent as Agent | undefined;

    let expectedProtocol = (defaultAgent as Agent).protocol;
    const protocol = options.protocol || expectedProtocol;
    if (this.agent?.protocol) expectedProtocol = this.agent.protocol;
    const defaultPort = options.defaultPort || this.agent?.defaultPort || 80;

    if (protocol !== expectedProtocol) {
      throw new ERR_INVALID_PROTOCOL(protocol, expectedProtocol);
    }
    this.protocol = protocol;
    const port = (options.port = options.port || defaultPort || 80);
    this.port = port.toString();
    const host = (options.host =
      validateHost(options.hostname, 'hostname') ||
      validateHost(options.host, 'host') ||
      'localhost');

    const setHost =
      options.setHost !== undefined
        ? Boolean(options.setHost) // eslint-disable-line @typescript-eslint/no-unnecessary-type-conversion
        : options.setDefaultHeaders !== false;
    if (options.timeout !== undefined)
      this.timeout = getTimerDuration(options.timeout, 'timeout');

    const signal = options.signal;
    if (signal) {
      addAbortSignal(signal, this);
    }
    let method = options.method;
    const methodIsString = typeof method === 'string';
    if (method != null && !methodIsString) {
      throw new ERR_INVALID_ARG_TYPE('options.method', 'string', method);
    }

    if (methodIsString && method) {
      if (!checkIsHttpToken(method)) {
        throw new ERR_INVALID_HTTP_TOKEN('Method', method);
      }
      method = this.method = method.toUpperCase();
    } else {
      method = this.method = 'GET';
    }

    const maxHeaderSize = options.maxHeaderSize;
    if (maxHeaderSize !== undefined) {
      // This overrides the maximum length of response headers in bytes.
      // It doesn't make sense to override the maximum length for Workerd implementation
      // which is based on the original "fetch" API.
      validateInteger(maxHeaderSize, 'maxHeaderSize', 0);
      throw new ERR_OPTION_NOT_IMPLEMENTED('options.maxHeaderSize');
    }

    if (options.insecureHTTPParser !== undefined) {
      // If enabled it will use a HTTP parser with leniency flags enabled.
      // Since our implementation does not use any http parser, and uses "fetch" API,
      // it doesn't make sense to support this option.
      validateBoolean(options.insecureHTTPParser, 'options.insecureHTTPParser');
      throw new ERR_OPTION_NOT_IMPLEMENTED('options.insecureHTTPParser');
    }

    if (options.createConnection !== undefined) {
      // Our implementation is based on the original "fetch" API, which doesn't support
      // custom socket creation. Therefore, this option is not applicable.
      validateFunction(options.createConnection, 'options.createConnection');
      throw new ERR_OPTION_NOT_IMPLEMENTED('options.createConnection');
    }

    if (options.lookup !== undefined) {
      // Our implementation is based on the original "fetch" API, which doesn't support
      // custom DNS resolution. Therefore, this option is not applicable.
      validateFunction(options.lookup, 'options.lookup');
      throw new ERR_OPTION_NOT_IMPLEMENTED('options.lookup');
    }

    if (options.socketPath !== undefined) {
      // Unix domain socket. Cannot be used if one of host or port is specified, as those specify a TCP Socket.
      // This option is not applicable for our "fetch" based implementation.
      validateString(options.socketPath, 'options.socketPath');
      throw new ERR_OPTION_NOT_IMPLEMENTED('options.socketPath');
    }

    if (options.joinDuplicateHeaders !== undefined) {
      validateBoolean(
        options.joinDuplicateHeaders,
        'options.joinDuplicateHeaders'
      );
    }
    this.joinDuplicateHeaders = options.joinDuplicateHeaders;

    this.path = options.path || '/';
    if (cb) {
      this.once('response', cb);
    }

    this.host = host;

    const headers = options.headers;
    if (!Array.isArray(headers)) {
      if (headers != null) {
        if ('host' in headers) {
          validateString(headers.host, 'host');
        }
        for (const [key, value] of Object.entries(headers)) {
          this.setHeader(key, value as unknown as string);
        }
      }

      if (host && !this.getHeader('host') && setHost) {
        let hostHeader = host;

        // For the Host header, ensure that IPv6 addresses are enclosed
        // in square brackets, as defined by URI formatting
        // https://tools.ietf.org/html/rfc3986#section-3.2.2
        const posColon = hostHeader.indexOf(':');
        if (
          posColon !== -1 &&
          hostHeader.includes(':', posColon + 1) &&
          hostHeader.charCodeAt(0) !== 91 /* '[' */
        ) {
          hostHeader = `[${hostHeader}]`;
        }

        if (port && +port !== defaultPort) {
          // eslint-disable-next-line @typescript-eslint/restrict-plus-operands
          hostHeader += ':' + port;
        }
        this.setHeader('Host', hostHeader);
      }

      if (options.auth && !this.getHeader('Authorization')) {
        this.setHeader(
          'Authorization',
          'Basic ' + Buffer.from(options.auth).toString('base64')
        );
      }
    } else {
      if (headers.length % 2 !== 0) {
        throw new ERR_INVALID_ARG_VALUE('headers', headers);
      }

      for (let n = 0; n < headers.length; n += 2) {
        this.setHeader(headers[n + 0] as string, headers[n + 1] as string);
      }
    }

    this.on('finish', () => {
      this.#onFinish();
    });

    this[kUniqueHeaders] = parseUniqueHeadersOption(options.uniqueHeaders);
  }

  #onFinish(): void {
    if (this.destroyed) return;

    let body: BodyInit | null = null;
    if (this.method !== 'GET' && this.method !== 'HEAD') {
      const value = this.getHeader('content-type') ?? '';
      body = new Blob(this.#body, {
        type: Array.isArray(value) ? value.join(', ') : `${value}`,
      });
    }

    const headers: [string, string][] = [];
    for (const [_lowerCaseName, [originalName, value]] of Object.entries(
      this[kOutHeaders] ?? {}
    )) {
      if (Array.isArray(value)) {
        if (this.joinDuplicateHeaders) {
          headers.push([originalName, value.join(', ')]);
        } else {
          for (const item of value) {
            headers.push([originalName, item]);
          }
        }
      } else {
        headers.push([originalName, value]);
      }
    }

    if (this.timeout) {
      this.#timer = setTimeout(() => {
        this.emit('timeout');
        this.#incomingMessage?.emit('timeout');
        this.#abortController.abort();
      }, this.timeout) as unknown as number;
    }

    if (
      this.host &&
      !this.getHeader('host') &&
      Object.keys(this[kOutHeaders] ?? {}).length === 0
    ) {
      // From RFC 7230 5.4 https://datatracker.ietf.org/doc/html/rfc7230#section-5.4
      // A server MUST respond with a 400 (Bad Request) status code to any
      // HTTP/1.1 request message that lacks a Host header field
      queueMicrotask(() => {
        this.#handleFetchResponse(
          new Response(null, {
            status: 400,
            statusText: 'Bad Request',
            headers: {
              connection: 'close',
            },
          })
        );
      });
      return;
    }

    const host = this.getHeader('host') ?? this.host;
    const url = new URL(`http://${host}`);
    url.protocol = this.protocol;
    url.port = this.port;
    url.pathname = this.path;

    // Our fetch implementation has the following limitations.
    //
    // 1. Content decoding is handled automatically by fetch,
    //    but expectation is that it's not handled in http.
    // 2. Nothing is directly waiting for fetch promise here.
    //    It's up to the user of the HTTP API to arrange for
    //    the request to be held open until the fetch completes,
    //    typically by passing some promise to ctx.waitUntil()
    //    and resolving that promise when the request is complete.
    //
    // TODO(soon): Address these concerns and limitations.
    fetch(url, {
      method: this.method,
      headers,
      body: body ?? null,
      signal: this.#abortController.signal,
      redirect: 'manual',
    })
      .then(this.#handleFetchResponse.bind(this))
      .catch(this.#handleFetchError.bind(this));
  }

  #handleFetchResponse(response: Response): void {
    // Sets headersSent
    this._header = Array.from(response.headers.keys())
      .map((key) => `${key}=${response.headers.get(key)}}`)
      .join('\r\n');
    const incoming = new IncomingMessage();
    setIncomingMessageFetchResponse(incoming, response);
    incoming.on('error', (error) => {
      this.emit('error', error);
    });

    this.emit('response', incoming);
    // @ts-expect-error TS2540 This is a read-only property.
    this.req = this.#incomingMessage;
    this.#incomingMessage = incoming;
  }

  #handleFetchError(error: Error): void {
    if (!this.destroyed) {
      this.emit('error', error);
    } else {
      console.log(error);
    }
    this.destroyed = true;
    this._ended = true;
  }

  abort(error?: Error | null): void {
    this.destroyed = true;
    this.#resetTimers({ finished: true });
    if (this.#incomingMessage) {
      this.#incomingMessage.destroyed = true;
    }
    this.#abortController.abort();
    if (error) {
      this.emit('error', error);
    }
  }

  override _write(
    chunk: Buffer,
    _encoding: BufferEncoding,
    callback: VoidFunction
  ): boolean {
    this.#body.push(chunk);
    callback();
    return true;
  }

  setNoDelay(noDelay?: boolean): void {
    validateBoolean(noDelay, 'noDelay');
    // Not implemented
  }

  setSocketKeepAlive(enable?: boolean, initialDelay?: number): void {
    validateBoolean(enable, 'enable');
    validateNumber(initialDelay, 'initialDelay');
    // Not implemented
  }

  clearTimeout(cb?: VoidFunction): void {
    this.setTimeout(0, cb);
  }

  setTimeout(msecs: number, callback?: VoidFunction): this {
    if (this.#timer) {
      clearTimeout(this.#timer);
      this.#timer = null;
    }

    this.timeout = getTimerDuration(msecs, 'msecs');
    this.#resetTimers({ finished: false });

    if (callback) this.once('timeout', callback);

    return this;
  }

  // @ts-expect-error TS2416 Type mismatch.
  override end(
    data?: Buffer | string | VoidFunction,
    encoding?: NodeJS.BufferEncoding,
    callback?: VoidFunction
  ): this {
    this._ended = true;

    if (typeof data === 'function') {
      callback = data as VoidFunction;
      data = undefined;
    }

    Writable.prototype.end.call(
      this,
      data,
      encoding as BufferEncoding,
      callback
    );
    return this;
  }

  #resetTimers({ finished }: { finished: boolean }): void {
    if (finished) {
      clearTimeout(this.#timer as number);
      this.#timer = null;
    } else if (this.timeout) {
      if (this.#timer) {
        clearTimeout(this.#timer);
      }
      this.#timer = setTimeout(() => {
        this.emit('timeout');
        this.#incomingMessage?.emit('timeout');
        this.#abortController.abort();
      }, this.timeout) as unknown as number;
    }
  }

  override _implicitHeader(): void {
    if (this._header) {
      throw new ERR_HTTP_HEADERS_SENT('render');
    }
    this._storeHeader(
      this.method + ' ' + this.path + ' HTTP/1.1\r\n',
      this[kOutHeaders] as OutgoingHttpHeaders
    );
  }
}
