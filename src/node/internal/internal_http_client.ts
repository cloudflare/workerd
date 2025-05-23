// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { _checkIsHttpToken as checkIsHttpToken } from 'node-internal:internal_http';
import { kOutHeaders } from 'node-internal:internal_http_outgoing';
import {
  validateHeaderName,
  validateHeaderValue,
} from 'node-internal:internal_http';
import { Buffer } from 'node-internal:internal_buffer';
import { urlToHttpOptions, isURL } from 'node-internal:internal_url';
import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_HTTP_TOKEN,
  ERR_OPTION_NOT_IMPLEMENTED,
  ERR_UNESCAPED_CHARACTERS,
} from 'node-internal:internal_errors';
import { validateInteger, validateBoolean } from 'node-internal:validators';
import { getTimerDuration } from 'node-internal:internal_net';
import { addAbortSignal } from 'node-internal:streams_util';
import { Writable } from 'node-internal:streams_writable';
import type {
  ClientRequest as _ClientRequest,
  RequestOptions as _RequestOptions,
} from 'node:http';
import { IncomingMessage } from 'node-internal:internal_http_incoming';

const INVALID_PATH_REGEX = /[^\u0021-\u00ff]/;

function validateHost(host: unknown, name: string): string {
  if (host !== null && host !== undefined && typeof host !== 'string') {
    throw new ERR_INVALID_ARG_TYPE(
      `options.${name}`,
      ['string', 'undefined', 'null'],
      host
    );
  }
  return host as string;
}

type Callback = (req: ClientRequest) => void;

export class ClientRequest extends Writable {
  public [kOutHeaders]: Record<string, { name: string; value: string }> = {};

  #abortController = new AbortController();
  #body: Buffer[] = [];
  #incomingMessage?: IncomingMessage;
  #timer: number | null = null;

  public _ended: boolean = false;

  public timeoutCb?: VoidFunction | undefined;
  public timeout?: number;
  public method: string = 'GET';
  public path: string;
  public host: string;

  public constructor(
    input: (string | URL | Record<string, unknown> | null) | _RequestOptions,
    options?: _RequestOptions | Callback,
    cb?: Callback
  ) {
    super();

    if (typeof input === 'string') {
      input = urlToHttpOptions(new URL(input));
    } else if (isURL(input)) {
      // url.URL instance
      input = urlToHttpOptions(input);
    } else {
      cb = options as Callback;
      options = input as _RequestOptions;
      input = null;
    }

    if (typeof options === 'function') {
      cb = options;
      options = input || {};
    } else {
      options = Object.assign(input || {}, options);
    }

    if (options.path) {
      const path = String(options.path);
      if (INVALID_PATH_REGEX.test(path)) {
        throw new ERR_UNESCAPED_CHARACTERS('Request path');
      }
    }

    const defaultPort = options.defaultPort || 80;

    const port = (options.port = options.port || defaultPort || 80);
    const host = (options.host =
      validateHost(options.hostname, 'hostname') ||
      validateHost(options.host, 'host') ||
      'localhost');

    const setHost =
      options.setHost !== undefined
        ? Boolean(options.setHost)
        : options.setDefaultHeaders !== false;

    if (options.timeout !== undefined)
      this.timeout = getTimerDuration(options.timeout, 'timeout');

    const signal = options.signal;
    if (signal) {
      // @ts-expect-error TS2379 Type inconsistency
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
      validateInteger(maxHeaderSize, 'maxHeaderSize', 0);
      throw new ERR_OPTION_NOT_IMPLEMENTED('options.maxHeaderSize');
    }

    if (options.insecureHTTPParser !== undefined) {
      throw new ERR_OPTION_NOT_IMPLEMENTED('options.insecureHTTPParser');
    }

    if (options.joinDuplicateHeaders !== undefined) {
      validateBoolean(
        options.joinDuplicateHeaders,
        'options.joinDuplicateHeaders'
      );

      // eslint-disable-next-line @typescript-eslint/no-unnecessary-boolean-literal-compare
      if (options.joinDuplicateHeaders === true) {
        // We do not support joinDuplicateHeaders because the underlying
        // implementation based on fetch() will handle duplicate headers.
        throw new ERR_OPTION_NOT_IMPLEMENTED('options.joinDuplicateHeaders');
      }
    }

    this.path = options.path || '/';
    if (cb) {
      this.once('response', cb);
    }

    this.host = host;

    const headers = options.headers;
    if (!Array.isArray(headers)) {
      if (headers != null) {
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
    }

    this.on('finish', () => {
      this.#onFinish();
    });
  }

  public setHeader(name: string, value?: string): void {
    validateHeaderName(name);
    validateHeaderValue(name, value);

    // Node.js uses an array, but we store it as an object.
    this[kOutHeaders][name.toLowerCase()] = {
      name,
      value: value as string,
    };
  }

  public getHeader(name: string): string | undefined {
    return this[kOutHeaders][name.toLowerCase()]?.value;
  }

  public removeHeader(name: string): void {
    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
    delete this[kOutHeaders][name.toLowerCase()];
  }

  #onFinish(): void {
    if (this.destroyed) return;

    let body: BodyInit | null = null;
    if (this.method !== 'GET' && this.method !== 'HEAD') {
      body = new Blob(this.#body, {
        type: this.getHeader('content-type') ?? '',
      });
    }

    const headers: [string, string][] = [];
    for (const name of Object.keys(this[kOutHeaders])) {
      const value = this[kOutHeaders][name]?.value;
      if (value) {
        if (Array.isArray(value)) {
          for (const item of value) {
            headers.push([name, item as string]);
          }
        } else {
          headers.push([name, value]);
        }
      }
    }

    const host = this.getHeader('host');
    if (!host) {
      // TODO(soon): Investigate what to do when this particular error is thrown.
      throw new Error('Host header is not set');
    }

    if (this.timeout) {
      this.#timer = setTimeout(() => {
        this.emit('timeout');
        this.#incomingMessage?.emit('timeout');
        this.#abortController.abort();
      }, this.timeout) as unknown as number;
    }

    const url = new URL(`http://${host}`);
    url.pathname = this.path;

    fetch(url, {
      method: this.method,
      headers,
      body: body ?? null,
      signal: this.#abortController.signal,
    })
      .then(this.#handleFetchResponse.bind(this))
      .catch(this.#handleFetchError.bind(this));
  }

  #handleFetchResponse(response: Response): void {
    this.#incomingMessage = new IncomingMessage(
      response,
      this.#resetTimers.bind(this)
    );
    this.#incomingMessage.on('error', (error) => {
      this.emit('error', error);
    });

    this.emit('response', this.#incomingMessage);
  }

  #handleFetchError(error: Error): void {
    if (!this.destroyed) {
      this.emit('error', error);
    } else {
      // Without this error log, it's impossible to debug.
      console.error(error);
    }
  }

  public abort(error?: Error | null): void {
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

  public override _write(
    chunk: Buffer,
    _encoding: BufferEncoding,
    callback: VoidFunction
  ): void {
    this.#body.push(chunk);
    callback();
  }

  public setNoDelay(_noDelay?: boolean): void {
    // Not implemented
  }

  public setSocketKeepAlive(_enable?: boolean, _initialDelay?: number): void {
    // Not implemented
  }

  public clearTimeout(cb?: VoidFunction): void {
    this.setTimeout(0, cb);
  }

  public setTimeout(msecs: number, callback?: VoidFunction): this {
    if (this._ended || this.timeoutCb) {
      return this;
    }

    if (this.#timer) {
      clearTimeout(this.#timer);
      this.#timer = null;
    }

    this.timeoutCb = callback;
    this.timeout = getTimerDuration(msecs, 'msecs');
    this.#resetTimers({ finished: false });

    if (callback) this.once('timeout', callback);

    return this;
  }

  // @ts-expect-error TS2416 Type mismatch.
  public override end(
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
      this.#timer = setTimeout(() => {
        this.emit('timeout');
      }, this.timeout) as unknown as number;
    }
  }

  public flushHeaders(): void {
    // Not implemented
  }
}
