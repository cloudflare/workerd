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
  ERR_METHOD_NOT_IMPLEMENTED,
  ConnResetException,
  AbortError,
} from 'node-internal:internal_errors';
import {
  validateInteger,
  validateBoolean,
  validateFunction,
  validateString,
  validateNumber,
} from 'node-internal:validators';
import { getTimerDuration } from 'node-internal:internal_net';
import { addAbortSignal } from 'node-internal:streams_add_abort_signal';
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
import {
  OutgoingMessage,
  kErrored,
} from 'node-internal:internal_http_outgoing';
import { Agent, globalAgent } from 'node-internal:internal_http_agent';
import type { IncomingMessageCallback } from 'node-internal:internal_http_util';
import type { Socket } from 'node:net';

const INVALID_PATH_REGEX = /[^\u0021-\u00ff]/;

// Matches paths that would override the URL authority when passed to
// `new URL(path, base)`: double separators (//  /\  \/  \\) or a scheme
// (colon before the first separator).
const AUTHORITY_OVERRIDE_REGEX = /^(?:[/\\]{2}|[^/\\]*:)/;

type WriteCallback = (err?: Error) => void;

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

// @ts-expect-error TS2720 Complaining due to "override req" being undefined.
export class ClientRequest extends OutgoingMessage implements _ClientRequest {
  #abortController = new AbortController();
  #body: (Buffer | Uint8Array)[] = [];
  #incomingMessage?: IncomingMessage;
  #timer: number | null = null;
  #sent = false;

  _ended: boolean = false;

  timeout?: number;
  method: string = 'GET';
  path: string = '/';
  host: string;
  protocol: string = 'http:';
  port: string = '80';
  joinDuplicateHeaders: boolean | undefined;
  agent: Agent | undefined;

  override aborted: boolean = false;

  // Unused fields required to be Node.js compatible.
  reusedSocket: boolean = false;
  maxHeadersCount: number = Infinity;
  connection: Socket | null = null;
  socket: Socket | null = null;

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
      // Reject paths that would override the URL authority when passed to
      // `new URL(path, base)`.  Two cases:
      //
      // 1. Network-path references and backslash variants — the WHATWG URL
      //    parser treats \ as / for special schemes, so any pair of / and \
      //    at the start (//  /\  \/  \\) introduces an authority.
      //
      // 2. Absolute-form URLs — a scheme (e.g. "http:") before the first
      //    separator causes the parser to ignore the base entirely.
      if (AUTHORITY_OVERRIDE_REGEX.test(options.path)) {
        throw new ERR_INVALID_ARG_VALUE(
          'options.path',
          options.path,
          'must be a path-only request target'
        );
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
      addAbortSignal(signal, this as unknown as Writable);
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
      if (this.#body.length > 0) {
        const value = this.getHeader('content-type') ?? '';
        body = new Blob(this.#body as BlobPart[], {
          type: Array.isArray(value) ? value.join(', ') : `${value}`,
        });
      }
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

    this.#sent = true;
    this.#armTimer();

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

    let url = new URL(`http://${this.host}`);
    url.protocol = this.protocol;
    url.port = this.port;

    if (this.path.length > 0 && this.path !== '/') {
      // Defense-in-depth: re-validate in case this.path was mutated after
      // construction (the field is public).
      if (AUTHORITY_OVERRIDE_REGEX.test(this.path)) {
        this.destroy(
          new ERR_INVALID_ARG_VALUE(
            'options.path',
            this.path,
            'must be a path-only request target'
          )
        );
        return;
      }
      // We pass `path` as the first argument since it can contain search and hash components.
      // Therefore, running the pathname setter will not work.
      // Since this is an extremely costly operation, we only do it if necessary.
      url = new URL(this.path, url);
    }

    // Our fetch implementation has the following limitation:
    //
    // Nothing is directly waiting for fetch promise here.
    // It's up to the user of the HTTP API to arrange for
    // the request to be held open until the fetch completes,
    // typically by passing some promise to ctx.waitUntil()
    // and resolving that promise when the request is complete.
    //
    // TODO(soon): Address this limitation.

    // We use encodeResponseBody: 'manual' to prevent fetch from automatically
    // decompressing the response body. Node.js http does not auto-decompress;
    // callers are expected to handle Content-Encoding themselves.
    // The type assertion is needed because the DOM RequestInit type does not
    // include the workerd-specific encodeResponseBody property.
    fetch(url, {
      method: this.method,
      headers,
      body: body ?? null,
      signal: this.#abortController.signal,
      redirect: 'manual',
      encodeResponseBody: 'manual',
    } as RequestInit & { encodeResponseBody: 'manual' })
      .then(this.#handleFetchResponse.bind(this))
      .catch(this.#handleFetchError.bind(this));
  }

  #handleFetchResponse(response: Response): void {
    // Destroyed while the response was on its way: nobody will read it.
    if (this.destroyed) {
      response.body?.cancel().catch(() => {});
      return;
    }

    // Sets headersSent
    this._header = Array.from(response.headers.keys())
      .map((key) => `${key}=${response.headers.get(key)}}`)
      .join('\r\n');
    const incoming = new IncomingMessage();
    setIncomingMessageFetchResponse(incoming, response);
    // The response's own failure (its body erroring, or its destroy()) is
    // the request's too, as a socket error would be in Node; a request
    // being destroyed reports its error itself.
    incoming.on('error', (error) => {
      if (!this.destroyed) {
        this.emit('error', error);
      }
    });
    // The exchange is over once the response is: the request closes.
    incoming.once('close', () => {
      this.#emitClose();
    });

    this.#incomingMessage = incoming;
    // @ts-expect-error TS2540 This is a read-only property.
    this.res = incoming;
    this.emit('response', incoming);
  }

  // A fetch that fails before yielding a response (the connection could
  // not be made, the request was rejected). The rejection of a fetch this
  // request aborted itself is not news.
  #handleFetchError(error: Error): void {
    if (this.destroyed) return;
    this.destroy(error);
  }

  // Marks the request closed and destroyed, once; the end of every
  // exchange, completed or torn down, comes through here.
  #emitClose(): void {
    if (this._closed) return;
    this._closed = true;
    this.destroyed = true;
    this.#clearTimer();
    this.emit('close');
  }

  // Tears the exchange down as Node does when the socket goes away. The
  // request reports `err` — or, for a bare destroy() before any response
  // (and not through abort()), that the connection hung up — on a later
  // tick, as a socket error would arrive; a response in flight is aborted
  // at once, with `err` or ECONNRESET 'aborted', which cancels its body so
  // the server learns of it; a fetch still awaiting its response is
  // aborted; 'close' follows.
  override destroy(err?: unknown, _cb?: (err?: unknown) => void): this {
    if (this.destroyed) return this;
    this.destroyed = true;
    this[kErrored] = (err as Error | null | undefined) ?? null;
    this.#clearTimer();

    const incoming = this.#incomingMessage;
    if (incoming === undefined && err == null && !this.aborted) {
      err = new ConnResetException('socket hang up');
    }
    if (err != null) {
      queueMicrotask(() => {
        this.emit('error', err);
      });
    }
    if (incoming === undefined) {
      this.#abortController.abort();
    } else if (!incoming.complete) {
      incoming.destroy(
        (err as Error | undefined) ?? new ConnResetException('aborted')
      );
    }
    queueMicrotask(() => {
      this.#emitClose();
    });
    return this;
  }

  onSocket(_socket: Socket): void {
    // Do nothing. Our implementation does not depend on socket class.
  }

  addTrailers(
    _headers: OutgoingHttpHeaders | ReadonlyArray<[string, string]>
  ): void {
    // We don't support trailers.
    throw new ERR_METHOD_NOT_IMPLEMENTED('addTrailers');
  }

  // The quiet teardown: 'abort' on the next tick, then the destroy() of a
  // bare request without its 'socket hang up'.
  abort(): void {
    if (this.aborted) return;
    this.aborted = true;
    queueMicrotask(() => {
      this.emit('abort');
    });
    this.destroy();
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

  // Arms (or, with 0, clears) the timeout, which runs from the moment the
  // request is sent and this method has been called, whichever is later.
  setTimeout(msecs: number, callback?: VoidFunction): this {
    this.timeout = getTimerDuration(msecs, 'msecs');
    this.#armTimer();

    if (callback) this.once('timeout', callback);

    return this;
  }

  override write(
    chunk: string | Buffer | Uint8Array,
    encoding?: BufferEncoding | WriteCallback | null,
    callback?: WriteCallback
  ): boolean {
    // Capture the data for the request body
    if (this.method !== 'GET' && this.method !== 'HEAD' && chunk) {
      if (typeof chunk === 'string') {
        this.#body.push(
          Buffer.from(chunk, typeof encoding === 'string' ? encoding : 'utf8')
        );
      } else {
        this.#body.push(chunk);
      }
    }

    // Call the parent write method
    return super.write(chunk, encoding, callback);
  }

  override end(
    data?: Buffer | string | VoidFunction,
    encoding?: BufferEncoding | VoidFunction,
    callback?: VoidFunction
  ): this {
    // A destroyed request has nothing left to send and never finishes.
    if (this.destroyed) return this;
    this._ended = true;

    if (typeof data === 'function') {
      callback = data as VoidFunction;
      data = undefined;
    }

    // Don't duplicate data here - let the parent's end() call write() which will handle it
    Writable.prototype.end.call(
      this,
      data,
      encoding as BufferEncoding,
      callback
    );
    return this;
  }

  #clearTimer(): void {
    if (this.#timer !== null) {
      clearTimeout(this.#timer);
      this.#timer = null;
    }
  }

  // One timer at a time, once the request has been sent. Firing emits
  // 'timeout' on the request and its response, then destroys the request
  // with an AbortError. (Node only emits and leaves the teardown to the
  // listener; a request left open would otherwise hold the fetch.)
  #armTimer(): void {
    this.#clearTimer();
    if (!this.timeout || !this.#sent || this.destroyed) return;
    this.#timer = setTimeout(() => {
      this.#timer = null;
      this.emit('timeout');
      this.#incomingMessage?.emit('timeout');
      this.destroy(new AbortError());
    }, this.timeout) as unknown as number;
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
