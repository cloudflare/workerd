// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { EventEmitter } from 'node-internal:events';
import { Readable } from 'node-internal:streams_readable';
import { isIPv4, Socket } from 'node-internal:internal_net';
import type {
  IncomingMessage as _IncomingMessage,
  IncomingHttpHeaders,
} from 'node:http';
const kHeaders = Symbol('kHeaders');
const kHeadersDistinct = Symbol('kHeadersDistinct');
const kHeadersCount = Symbol('kHeadersCount');

export let setIncomingMessageFetchResponse: (
  incoming: IncomingMessage,
  response: Response,
  resetTimers?: (opts: { finished: boolean }) => void
) => void;

export let setIncomingMessageSocket: (
  incoming: IncomingMessage,
  options: {
    headers: Headers;
    localPort: number;
  }
) => void;

export let setIncomingRequestBody: (
  incoming: IncomingMessage,
  body: ReadableStream | null
) => void;

export class IncomingMessage extends Readable implements _IncomingMessage {
  #response?: Response;
  #reader?: ReadableStreamDefaultReader<Uint8Array>;
  #reading = false;
  #socket: unknown;
  #stream: ReadableStream | null = null;

  override aborted = false;
  url: string = '';
  // @ts-expect-error TS2416 Type-inconsistencies
  method: string | null = null;
  // @ts-expect-error TS2416 Type-inconsistencies
  statusCode: number | null = null;
  // @ts-expect-error TS2416 Type-inconsistencies
  statusMessage: string | null = null;
  httpVersionMajor = 1;
  httpVersionMinor = 1;
  httpVersion: string = '1.1';
  complete = false;
  rawHeaders: string[] = [];
  joinDuplicateHeaders = false;

  // The cloudflare property is currently only used on the server-side
  // to access properties like `req.cf`, and the `env` and `ctx`
  // objects.
  cloudflare: {
    // Technically, the type should be IncomingRequestCfProperties but
    // we don't have that type in the workerd runtime at the moment.
    cf?: Record<string, unknown> | undefined;
    env?: unknown;
    ctx?: unknown;
  } = { cf: undefined, env: undefined, ctx: undefined };

  [kHeaders]: IncomingHttpHeaders | null = null;
  [kHeadersDistinct]: Record<string, string[]> | null = null;
  [kHeadersCount]: number = 0;

  // Flag for when we decide that this message cannot possibly be
  // read by the user, so there's no point continuing to handle it.
  _dumped = false;
  _consuming = false;
  _paused = false;

  static {
    setIncomingMessageFetchResponse = (
      incoming: IncomingMessage,
      response: Response
    ): void => {
      incoming.#setFetchResponse(response);
    };

    // This method sets the socket property of the IncomingMessage object.
    // Please rest assured that this method implements a subset of Socket since
    // in Node.js it's net.Socket which isn't possible to implement within our own
    // implementation since our implementation is based on Request and Response objects.
    setIncomingMessageSocket = (
      incoming: IncomingMessage,
      { headers, localPort }: { headers: Headers; localPort: number }
    ): void => {
      const connectingIp = headers.get('cf-connecting-ip');
      const isConnectingIpIpv4 = connectingIp ? isIPv4(connectingIp) : true;
      // Return a port number between 2^15 and 2^16.
      const remotePort = (Math.random() * 0x8000) | 0x8000;

      // Some libraries such as on-finished (which Express.js depends on)
      // Ref: https://github.com/jshttp/on-finished/blob/d2974f5a18f468ea56f58acb2f6d402f4b5142f0/index.js
      // calls EventEmitter events on socket attribute.
      const socket = new EventEmitter();

      Object.defineProperties(socket, {
        encrypted: {
          value: headers.get('x-forwarded-proto') === 'https',
          writable: false,
          configurable: true,
        },
        readable: {
          get: () => {
            return incoming.readable;
          },
          configurable: true,
        },
        remoteFamily: {
          get: () => {
            if (incoming.destroyed) {
              return undefined;
            }
            return isConnectingIpIpv4 ? 'IPv4' : 'IPv6';
          },
          configurable: true,
        },
        remoteAddress: {
          get: () => {
            // This is defined in production, and will fallback to localhost on local development
            // where request headers does not contain cf-connecting-ip.
            return incoming.destroyed
              ? undefined
              : (connectingIp ?? '127.0.0.1');
          },
          configurable: true,
        },
        remotePort: {
          get: () => {
            // Return a port in the ephemeral range (32768-65535) as clients would use,
            // and undefined if the socket is destroyed.
            return incoming.destroyed ? undefined : remotePort;
          },
          configurable: true,
        },
        localAddress: {
          // Host will have a value like "my-worker.yagiz.workers.dev",
          value: headers.get('host') ?? '127.0.0.1',
          writable: false,
          configurable: true,
        },
        localPort: {
          // This is the port defined by the `server.listen(port)` call.
          value: localPort,
          writable: false,
          configurable: true,
        },
        destroy: {
          value: (err: Error | undefined): IncomingMessage =>
            incoming.destroy(err),
          writable: false,
          configurable: true,
        },
      });

      incoming.#socket = socket;
    };

    setIncomingRequestBody = (
      incoming: IncomingMessage,
      stream: ReadableStream | null
    ): void => {
      incoming.#stream = stream;
    };
  }

  constructor() {
    super({});
    this._readableState.readingMore = true;
  }

  #setFetchResponse(response: Response): void {
    this[kHeaders] = {};
    this[kHeadersDistinct] = {};
    for (const header of response.headers.keys()) {
      const value = response.headers.get(header) as string;
      this[kHeaders][header] = value;
      this[kHeadersDistinct][header] = [value];
      this[kHeadersCount]++;
    }

    this.#response = response;
    this._readableState.readingMore = true;

    this.url = response.url;
    this.statusCode = response.status;
    this.statusMessage = response.statusText;

    this.once('end', () => {
      // We need to emit close in a queueMicrotask because
      // this is the only way we can ensure that the close event is emitted after destroy.
      queueMicrotask(() => this.emit('close'));
    });

    this.on('timeout', () => {
      this._consuming = false;
    });

    this.#stream = this.#response.body;
  }

  async #tryRead(): Promise<void> {
    if (this.#stream == null || this.#reading) return;

    this.#reading = true;

    try {
      this.#reader ??= this.#stream.getReader();

      while (!this.destroyed) {
        const data = await this.#reader.read();
        if (data.done) {
          this.complete = true;
          this.push(null);
          break;
        }

        // Backpressure - stop reading until _read() is called again
        if (!this.push(data.value)) {
          break;
        }
      }
    } catch (e) {
      this.destroy(e as Error);
    } finally {
      this.#reading = false;
      this.#reader?.releaseLock();
    }
  }

  // As this is an implementation of stream.Readable, we provide a _read()
  // function that pumps the next chunk out of the underlying ReadableStream.
  override _read(_n: number): void {
    if (!this._consuming) {
      this._readableState.readingMore = false;
      this._consuming = true;
    }

    // Difference from Node.js -
    // The Node.js implementation will already have its internal buffer
    // filled by the parserOnBody function.
    // For our implementation, we use the ReadableStream instance.
    if (this.#stream == null) {
      // For GET and HEAD requests, the stream would be empty.
      // Simply signal that we're done.
      this.complete = true;
      this.push(null);
      return;
    }

    this.#tryRead(); // eslint-disable-line @typescript-eslint/no-floating-promises
  }

  #onError(error: Error | null, cb: (err?: Error | null) => void): void {
    // This is to keep backward compatible behavior.
    // An error is emitted only if there are listeners attached to the event.
    if (this.listenerCount('error') === 0) {
      cb();
    } else {
      cb(error);
    }
  }

  override _destroy(
    error: Error | null,
    callback: (error?: Error | null) => void
  ): void {
    if (!this.readableEnded || !this.complete) {
      this.aborted = true;
      this.emit('aborted');
    }

    queueMicrotask(() => {
      this.#onError(error, callback);
    });
  }

  // Add the given (field, value) pair to the message
  //
  // Per RFC2616, section 4.2 it is acceptable to join multiple instances of the
  // same header with a ', ' if the header in question supports specification of
  // multiple values this way. The one exception to this is the Cookie header,
  // which has multiple values joined with a '; ' instead. If a header's values
  // cannot be joined in either of these ways, we declare the first instance the
  // winner and drop the second. Extended header fields (those beginning with
  // 'x-') are always joined.
  _addHeaderLine(
    field: string,
    value: string,
    dest: IncomingHttpHeaders
  ): void {
    field = matchKnownFields(field);
    const flag = field.charCodeAt(0);
    if (flag === 0 || flag === 2) {
      field = field.slice(1);
      // Make a delimited list
      if (typeof dest[field] === 'string') {
        // eslint-disable-next-line @typescript-eslint/restrict-plus-operands
        dest[field] += (flag === 0 ? ', ' : '; ') + value;
      } else {
        dest[field] = value;
      }
    } else if (flag === 1) {
      // Array header -- only Set-Cookie at the moment
      if (dest['set-cookie'] !== undefined) {
        dest['set-cookie'].push(value);
      } else {
        dest['set-cookie'] = [value];
      }
    } else if (this.joinDuplicateHeaders) {
      // RFC 9110 https://www.rfc-editor.org/rfc/rfc9110#section-5.2
      // https://github.com/nodejs/node/issues/45699
      // allow authorization multiple fields
      // Make a delimited list
      if (dest[field] === undefined) {
        dest[field] = value;
      } else {
        // eslint-disable-next-line @typescript-eslint/restrict-plus-operands
        dest[field] += ', ' + value;
      }
    } else if (dest[field] === undefined) {
      // Drop duplicates
      dest[field] = value;
    }
  }

  _addHeaderLines(headers: string[] | null, n: number): void {
    if (Array.isArray(headers) && !this.complete) {
      this.rawHeaders = headers;
      this[kHeadersCount] = n;

      if (this[kHeaders]) {
        for (let i = 0; i < n; i += 2) {
          this._addHeaderLine(
            headers[i] as string,
            headers[i + 1] as string,
            this[kHeaders]
          );
        }
      }
    }
  }

  get headers(): Record<string, string | string[] | undefined> {
    if (!this[kHeaders]) {
      this[kHeaders] = {};

      const src = this.rawHeaders;
      const dst = this[kHeaders];

      for (let n = 0; n < this[kHeadersCount]; n += 2) {
        this._addHeaderLine(src[n] as string, src[n + 1] as string, dst);
      }
    }
    return this[kHeaders];
  }

  set headers(val: IncomingHttpHeaders) {
    this[kHeaders] = val;
  }

  get headersDistinct(): Record<string, string[]> {
    if (!this[kHeadersDistinct]) {
      this[kHeadersDistinct] = {};

      const src = this.rawHeaders;
      const dst = this[kHeadersDistinct];

      for (let n = 0; n < this[kHeadersCount]; n += 2) {
        this._addHeaderLineDistinct(
          src[n] as string,
          src[n + 1] as string,
          dst
        );
      }
    }
    return this[kHeadersDistinct];
  }

  set headersDistinct(val: Record<string, string[]>) {
    this[kHeadersDistinct] = val;
  }

  get trailers(): Record<string, string | undefined> {
    return {};
  }

  set trailers(_val: NodeJS.Dict<string>) {
    // Workerd doesn't support trailers.
  }

  get trailersDistinct(): Record<string, string[]> {
    return {};
  }

  set trailersDistinct(_val: Record<string, string[]>) {
    // Workerd doesn't support trailers.
  }

  _addHeaderLineDistinct(
    field: string,
    value: string,
    dest: Record<string, string[]>
  ): void {
    field = field.toLowerCase();
    if (!dest[field]) {
      dest[field] = [value];
    } else {
      dest[field]?.push(value);
    }
  }

  // Call this instead of resume() if we want to just
  // dump all the data to /dev/null
  _dump(): void {
    if (!this._dumped) {
      this._dumped = true;
      // If there is buffered data, it may trigger 'data' events.
      // Remove 'data' event listeners explicitly.
      this.removeAllListeners('data');
      this.resume();
    }
  }

  setTimeout(_msecs: number, callback?: () => void): this {
    if (callback) {
      this.on('timeout', callback);
    }
    return this;
  }

  override pipe<T extends NodeJS.WritableStream>(
    destination: T,
    options?: { end?: boolean }
  ): T {
    const shouldEnd = options?.end !== false;

    // Handle the piping manually for better control
    this.on('data', (chunk: string | Uint8Array) => {
      destination.write(chunk);
    });

    this.once('end', () => {
      if (shouldEnd) {
        destination.end();
      }
    });

    this.once('error', (err: unknown) => {
      destination.emit('error', err);
    });

    // Always ensure reading starts - call resume to trigger the stream
    this.resume();

    return destination;
  }

  set connection(value: unknown) {
    this.#socket = value;
  }

  get connection(): Socket {
    return this.#socket as Socket;
  }

  get socket(): Socket {
    return this.#socket as Socket;
  }
}

// This function is used to help avoid the lowercasing of a field name if it
// matches a 'traditional cased' version of a field name. It then returns the
// lowercased name to both avoid calling toLowerCase() a second time and to
// indicate whether the field was a 'no duplicates' field. If a field is not a
// 'no duplicates' field, a `0` byte is prepended as a flag. The one exception
// to this is the Set-Cookie header which is indicated by a `1` byte flag, since
// it is an 'array' field and thus is treated differently in _addHeaderLines().
// TODO: perhaps http_parser could be returning both raw and lowercased versions
// of known header names to avoid us having to call toLowerCase() for those
// headers.
function matchKnownFields(field: string, lowercased: boolean = false): string {
  switch (field.length) {
    case 3:
      if (field === 'Age' || field === 'age') return 'age';
      break;
    case 4:
      if (field === 'Host' || field === 'host') return 'host';
      if (field === 'From' || field === 'from') return 'from';
      if (field === 'ETag' || field === 'etag') return 'etag';
      if (field === 'Date' || field === 'date') return '\u0000date';
      if (field === 'Vary' || field === 'vary') return '\u0000vary';
      break;
    case 6:
      if (field === 'Server' || field === 'server') return 'server';
      if (field === 'Cookie' || field === 'cookie') return '\u0002cookie';
      if (field === 'Origin' || field === 'origin') return '\u0000origin';
      if (field === 'Expect' || field === 'expect') return '\u0000expect';
      if (field === 'Accept' || field === 'accept') return '\u0000accept';
      break;
    case 7:
      if (field === 'Referer' || field === 'referer') return 'referer';
      if (field === 'Expires' || field === 'expires') return 'expires';
      if (field === 'Upgrade' || field === 'upgrade') return '\u0000upgrade';
      break;
    case 8:
      if (field === 'Location' || field === 'location') return 'location';
      if (field === 'If-Match' || field === 'if-match') return '\u0000if-match';
      break;
    case 10:
      if (field === 'User-Agent' || field === 'user-agent') return 'user-agent';
      if (field === 'Set-Cookie' || field === 'set-cookie') return '\u0001';
      if (field === 'Connection' || field === 'connection')
        return '\u0000connection';
      break;
    case 11:
      if (field === 'Retry-After' || field === 'retry-after')
        return 'retry-after';
      break;
    case 12:
      if (field === 'Content-Type' || field === 'content-type')
        return 'content-type';
      if (field === 'Max-Forwards' || field === 'max-forwards')
        return 'max-forwards';
      break;
    case 13:
      if (field === 'Authorization' || field === 'authorization')
        return 'authorization';
      if (field === 'Last-Modified' || field === 'last-modified')
        return 'last-modified';
      if (field === 'Cache-Control' || field === 'cache-control')
        return '\u0000cache-control';
      if (field === 'If-None-Match' || field === 'if-none-match')
        return '\u0000if-none-match';
      break;
    case 14:
      if (field === 'Content-Length' || field === 'content-length')
        return 'content-length';
      break;
    case 15:
      if (field === 'Accept-Encoding' || field === 'accept-encoding')
        return '\u0000accept-encoding';
      if (field === 'Accept-Language' || field === 'accept-language')
        return '\u0000accept-language';
      if (field === 'X-Forwarded-For' || field === 'x-forwarded-for')
        return '\u0000x-forwarded-for';
      break;
    case 16:
      if (field === 'Content-Encoding' || field === 'content-encoding')
        return '\u0000content-encoding';
      if (field === 'X-Forwarded-Host' || field === 'x-forwarded-host')
        return '\u0000x-forwarded-host';
      break;
    case 17:
      if (field === 'If-Modified-Since' || field === 'if-modified-since')
        return 'if-modified-since';
      if (field === 'Transfer-Encoding' || field === 'transfer-encoding')
        return '\u0000transfer-encoding';
      if (field === 'X-Forwarded-Proto' || field === 'x-forwarded-proto')
        return '\u0000x-forwarded-proto';
      break;
    case 19:
      if (field === 'Proxy-Authorization' || field === 'proxy-authorization')
        return 'proxy-authorization';
      if (field === 'If-Unmodified-Since' || field === 'if-unmodified-since')
        return 'if-unmodified-since';
      break;
  }
  if (lowercased) {
    return '\u0000' + field;
  }
  return matchKnownFields(field.toLowerCase(), true);
}
