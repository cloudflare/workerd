// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

// We have deprecations because @types/node defines this.finished as deprecated.
/* eslint-disable @typescript-eslint/no-deprecated */

import { validateString } from 'node-internal:validators';
import { Writable } from 'node-internal:streams_writable';
import { ok } from 'node-internal:internal_assert';
import { getDefaultHighWaterMark } from 'node-internal:streams_util';
import type { DataWrittenEvent } from 'node-internal:internal_http_server';
import {
  ERR_HTTP_HEADERS_SENT,
  ERR_INVALID_ARG_TYPE,
  ERR_STREAM_CANNOT_PIPE,
  ERR_STREAM_DESTROYED,
  ERR_METHOD_NOT_IMPLEMENTED,
  ERR_STREAM_WRITE_AFTER_END,
  ERR_HTTP_CONTENT_LENGTH_MISMATCH,
  ERR_HTTP_BODY_NOT_ALLOWED,
  ERR_STREAM_NULL_VALUES,
  ERR_STREAM_ALREADY_FINISHED,
  ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';
import { isUint8Array } from 'node-internal:internal_types';
import {
  validateHeaderName,
  validateHeaderValue,
  chunkExpression as RE_TE_CHUNKED,
  utcDate,
} from 'node-internal:internal_http';
import { IncomingMessage } from 'node-internal:internal_http_incoming';
import { EventEmitter } from 'node-internal:events';
import type {
  OutgoingMessage as _OutgoingMessage,
  OutgoingHttpHeaders,
  ServerResponse,
  OutgoingHttpHeader,
} from 'node:http';

type WriteCallback = (err?: Error) => void;
export type OutputData = {
  data: string | Buffer | Uint8Array | null;
  encoding?: BufferEncoding | null | undefined;
  callback?: WriteCallback | null | undefined;
};
export type WrittenDataBufferEntry = OutputData & {
  length: number;
  written: boolean;
};
export type HeadersSentEvent = {
  statusCode: number;
  statusMessage: string;
  headers: Headers;
};

export const kUniqueHeaders = Symbol('kUniqueHeaders');
export const kHighWaterMark = Symbol('kHighWaterMark');
export const kNeedDrain = Symbol('kNeedDrain');
export const kOutHeaders = Symbol('kOutHeaders');
export const kErrored = Symbol('kErrored');
const kCorked = Symbol('corked');
const kChunkedBuffer = Symbol('kChunkedBuffer');
const kChunkedLength = Symbol('kChunkedLength');
const kBytesWritten = Symbol('kBytesWritten');
const kRejectNonStandardBodyWrites = Symbol('kRejectNonStandardBodyWrites');

const RE_CONN_CLOSE = /(?:^|\W)close(?:$|\W)/i;

type HeaderState = {
  connection: boolean;
  contLen: boolean;
  te: boolean;
  date: boolean;
  expect: boolean;
  trailer: boolean;
  header: string;
};

export function parseUniqueHeadersOption(
  headers?: (string | string[])[]
): Set<string> | null {
  if (!Array.isArray(headers)) {
    return null;
  }

  const unique = new Set<string>();
  for (const header of headers) {
    if (Array.isArray(header)) {
      for (const h of header) {
        unique.add(h.toLowerCase());
      }
    } else {
      unique.add(header.toLowerCase());
    }
  }
  return unique;
}

// Most of the code in this class is derived from Michael Hart's project
// Ref: https://github.com/mhart/fetch-to-node/blob/main/src/fetch-to-node/http-outgoing.ts
class MessageBuffer {
  #corked = 0;
  #index = 0;
  #onWrite: (data: DataWrittenEvent[]) => void;
  #bufferedWrites: { index: number; entry: WrittenDataBufferEntry }[] = [];
  #highWaterMark: number;

  constructor(
    onWrite: (data: DataWrittenEvent[]) => void,
    options: { highWaterMark: number }
  ) {
    this.#onWrite = onWrite;
    this.#highWaterMark = options.highWaterMark;
  }

  write(
    data: WrittenDataBufferEntry['data'],
    encoding: WrittenDataBufferEntry['encoding'],
    callback: WrittenDataBufferEntry['callback']
  ): boolean {
    const entry: WrittenDataBufferEntry = {
      data,
      length: data?.length ?? 0,
      encoding,
      callback,
      written: true,
    };

    const index = this.#index++;

    if (this.#corked === 0) {
      this.#onWrite([{ index, entry }]);
      queueMicrotask(() => {
        callback?.();
      });
    } else {
      // Buffer the write when corked
      this.#bufferedWrites.push({ index, entry });
      queueMicrotask(() => {
        callback?.();
      });
    }

    return true;
  }

  cork(): void {
    this.#corked++;
  }

  uncork(): void {
    this.#corked--;
    this._flush();
  }

  _flush(): void {
    // If fully uncorked, flush all buffered writes
    if (this.#corked <= 0) {
      this.#onWrite(this.#bufferedWrites.splice(0));
    }
  }

  get writableLength(): number {
    return this.#bufferedWrites.reduce((total, { entry }) => {
      return total + (entry.length || 0);
    }, 0);
  }

  get writableHighWaterMark(): number {
    return this.#highWaterMark;
  }

  get writableCorked(): number {
    return this.#corked;
  }
}

export type OutgoingMessageOptions = {
  highWaterMark?: number | undefined;
  rejectNonStandardBodyWrites?: boolean | undefined;
};

// Most of the code in this class is derived from Michael Hart's project
// Ref: https://github.com/mhart/fetch-to-node/blob/main/src/fetch-to-node/http-outgoing.ts
export class OutgoingMessage extends Writable implements _OutgoingMessage {
  [kOutHeaders]: Record<string, [string, string | string[]]> | null = null;
  [kErrored]: Error | null = null;
  [kCorked] = 0;
  [kChunkedBuffer]: OutputData[] = [];
  [kChunkedLength]: number = 0;
  [kNeedDrain] = false;
  [kRejectNonStandardBodyWrites]: boolean;
  [kHighWaterMark]: number;
  [kBytesWritten] = 0;

  // @ts-expect-error TS2416 IncomingMessage is not feature complete yet.
  readonly req?: IncomingMessage | undefined;
  #buffer: MessageBuffer | undefined | null;

  // Queue that holds all currently pending data, until the response will be
  // assigned to the socket (until it will its turn in the HTTP pipeline).
  outputData: OutputData[] = [];

  // `outputSize` is an approximate measure of how much data is queued on this
  // response. `_onPendingData` will be invoked to update similar global
  // per-connection counter. That counter will be used to pause/unpause the
  // TCP socket and HTTP Parser and thus handle the backpressure.
  outputSize: number = 0;

  // `writtenHeaderBytes` is the number of bytes the header has taken.
  // Since Node.js writes both the headers and body into the same outgoing
  // stream, it helps to keep track of this so that we can skip that many bytes
  // from the beginning of the stream when providing the outgoing stream.
  writtenHeaderBytes = 0;

  strictContentLength = false;
  chunkedEncoding = false;
  sendDate = false;
  shouldKeepAlive = true;
  override writable = true;
  finished = false;
  override destroyed = false;
  useChunkedEncodingByDefault = true;
  maxRequestsOnConnectionReached = false;

  // These are attributes provided by the Node.js implementation.
  _closed = false;
  _headerSent = false;
  _onPendingData: (delta: number) => void = () => {};
  _header: string | null = null;
  _contentLength: number | null = null;
  _hasBody = true;
  _removedContLen = false;
  _removedConnection = false;
  _removedTE = false;
  _last = false;
  _defaultKeepAlive = true;
  _maxRequestsPerSocket: number | undefined;
  _keepAliveTimeout = 0;

  constructor(req?: IncomingMessage, options?: OutgoingMessageOptions) {
    super();
    this.req = req;
    this[kHighWaterMark] = options?.highWaterMark ?? getDefaultHighWaterMark();
    this[kRejectNonStandardBodyWrites] =
      options?.rejectNonStandardBodyWrites ?? false;
    this.#buffer = new MessageBuffer(this.#onDataWritten.bind(this), {
      highWaterMark: this[kHighWaterMark],
    });

    this.once('end', () => {
      // We need to emit close in a queueMicrotask because
      // this is the only way we can ensure that the close event is emitted after destroy.
      queueMicrotask(() => {
        this._closed = true;
        this.emit('close');
      });
    });
  }

  #onDataWritten(data: DataWrittenEvent[]): void {
    this.emit('_dataWritten', data);
  }

  override cork(): void {
    this[kCorked]++;
    this.#buffer?.cork();
  }

  override uncork(): void {
    this[kCorked]--;
    this.#buffer?.uncork();

    if (this[kCorked] || this[kChunkedBuffer].length === 0) {
      return;
    }

    for (const { data, encoding, callback } of this[kChunkedBuffer]) {
      this._send(data ?? '', encoding, callback);
    }

    this[kChunkedBuffer].length = 0;
    this[kChunkedLength] = 0;
  }

  _storeHeader(
    firstLine: string,
    headers: OutgoingHttpHeaders | OutgoingHttpHeader[] | null
  ): void {
    // firstLine in the case of request is: 'GET /index.html HTTP/1.1\r\n'
    // in the case of response it is: 'HTTP/1.1 200 OK\r\n'
    const state: HeaderState = {
      connection: false,
      contLen: false,
      te: false,
      date: false,
      expect: false,
      trailer: false,
      header: firstLine,
    };

    if (headers != null) {
      if (headers === this[kOutHeaders]) {
        for (const key in headers) {
          const entry = headers[key] as [string, string];
          processHeader(this, state, entry[0], entry[1], false);
        }
      } else if (Array.isArray(headers)) {
        if (headers.length && Array.isArray(headers[0])) {
          for (let i = 0; i < headers.length; i++) {
            const entry = headers[i] as unknown as [string, string];
            processHeader(this, state, entry[0], entry[1], true);
          }
        } else {
          if (headers.length % 2 !== 0) {
            throw new ERR_INVALID_ARG_VALUE('headers', headers);
          }

          for (let n = 0; n < headers.length; n += 2) {
            processHeader(
              this,
              state,
              headers[n] as string,
              headers[n + 1] as string,
              true
            );
          }
        }
      } else {
        for (const key in headers) {
          // eslint-disable-next-line no-prototype-builtins
          if (headers.hasOwnProperty(key)) {
            const _headers = headers;
            processHeader(
              this,
              state,
              key,
              _headers[key] as OutgoingHttpHeader,
              true
            );
          }
        }
      }
    }

    let { header } = state;

    // Date header
    if (this.sendDate && !state.date) {
      header += 'Date: ' + utcDate() + '\r\n';
    }

    // Force the connection to close when the response is a 204 No Content or
    // a 304 Not Modified and the user has set a "Transfer-Encoding: chunked"
    // header.
    //
    // RFC 2616 mandates that 204 and 304 responses MUST NOT have a body but
    // node.js used to send out a zero chunk anyway to accommodate clients
    // that don't have special handling for those responses.
    //
    // It was pointed out that this might confuse reverse proxies to the point
    // of creating security liabilities, so suppress the zero chunk and force
    // the connection to close.
    if (
      this.chunkedEncoding &&
      ((this as unknown as ServerResponse).statusCode === 204 ||
        (this as unknown as ServerResponse).statusCode === 304)
    ) {
      this.chunkedEncoding = false;
      this.shouldKeepAlive = false;
    }

    // keep-alive logic
    if (this._removedConnection) {
      // shouldKeepAlive is generally true for HTTP/1.1. In that common case,
      // even if the connection header isn't sent, we still persist by default.
      this._last = !this.shouldKeepAlive;
    } else if (!state.connection) {
      const shouldSendKeepAlive =
        this.shouldKeepAlive &&
        (state.contLen ||
          this.useChunkedEncodingByDefault ||
          (this as unknown as { agent: unknown }).agent);
      if (shouldSendKeepAlive && this.maxRequestsOnConnectionReached) {
        header += 'Connection: close\r\n';
      } else if (shouldSendKeepAlive) {
        header += 'Connection: keep-alive\r\n';
        if (this._keepAliveTimeout && this._defaultKeepAlive) {
          const timeoutSeconds = Math.floor(this._keepAliveTimeout / 1000);
          let max = '';
          if (
            this._maxRequestsPerSocket != null &&
            ~~this._maxRequestsPerSocket > 0
          ) {
            max = `, max=${this._maxRequestsPerSocket}`;
          }
          header += `Keep-Alive: timeout=${timeoutSeconds}${max}\r\n`;
        }
      } else {
        this._last = true;
        header += 'Connection: close\r\n';
      }
    }

    if (!state.contLen && !state.te) {
      if (!this._hasBody) {
        // Make sure we don't end the 0\r\n\r\n at the end of the message.
        this.chunkedEncoding = false;
      } else if (!this.useChunkedEncodingByDefault) {
        this._last = true;
      } else if (
        !state.trailer &&
        !this._removedContLen &&
        typeof this._contentLength === 'number'
      ) {
        // eslint-disable-next-line @typescript-eslint/restrict-plus-operands
        header += 'Content-Length: ' + this._contentLength + '\r\n';
      } else if (!this._removedTE) {
        header += 'Transfer-Encoding: chunked\r\n';
        this.chunkedEncoding = true;
      } else {
        // We can't keep alive in this case, because with no header info the body
        // is defined as all data until the connection is closed.
        this._last = true;
      }
    }

    this._header = header + '\r\n';
    this._headerSent = false;

    // Wait until the first body chunk, or close(), is sent to flush,
    // UNLESS we're sending Expect: 100-continue.
    if (state.expect) this._send('');
  }

  _finish(): void {
    this.emit('prefinish');
  }

  _flushOutput(buffer: MessageBuffer): boolean | undefined {
    const outputData = this.outputData;
    if (outputData.length === 0) {
      return undefined;
    }

    buffer.cork();
    for (const { data, encoding, callback } of outputData) {
      buffer.write(data, encoding, callback);
    }
    buffer.uncork();

    this.outputData = [];
    this._onPendingData(-this.outputSize);
    this.outputSize = 0;

    return true;
  }

  _flush(): void {
    if (this.#buffer != null) {
      const ret = this._flushOutput(this.#buffer);

      if (this.finished) {
        this._finish();
      } else if (ret && this[kNeedDrain]) {
        this[kNeedDrain] = false;
        this.emit('drain');
      }
    }
  }

  // @ts-expect-error TS2611 Required for accessor
  get writableLength(): number {
    return this.outputSize + this[kChunkedLength];
  }

  // @ts-expect-error TS2611 Required for accessor
  get writableCorked(): number {
    return this[kCorked];
  }

  // @ts-expect-error TS2611 Required for accessor
  get writableNeedDrain(): boolean {
    return !this.destroyed && !this.finished && this[kNeedDrain];
  }

  setHeader(name: string, value: number | string | string[]): this {
    if (this._header) {
      throw new ERR_HTTP_HEADERS_SENT('set');
    }
    validateHeaderName(name);
    validateHeaderValue(name, value);

    let headers = this[kOutHeaders];
    if (headers === null) {
      this[kOutHeaders] = headers = {};
    }

    headers[name.toLowerCase()] = [name, value];
    return this;
  }
  setHeaders(
    headers: Headers | Map<string, string | number | readonly string[]>
  ): this {
    if (this.headersSent) {
      throw new ERR_HTTP_HEADERS_SENT('set');
    }

    if (
      Array.isArray(headers) ||
      typeof headers !== 'object' ||
      !('keys' in headers) ||
      !('get' in headers) ||
      typeof headers.keys !== 'function' ||
      typeof headers.get !== 'function'
    ) {
      throw new ERR_INVALID_ARG_TYPE('headers', ['Headers', 'Map'], headers);
    }

    // Headers object joins multiple cookies with a comma when using
    // the getter to retrieve the value,
    // unless iterating over the headers directly.
    // We also cannot safely split by comma.
    // To avoid setHeader overwriting the previous value we push
    // set-cookie values in array and set them all at once.
    const cookies: string[] = [];

    for (const { 0: key, 1: value } of headers) {
      if (key === 'set-cookie') {
        if (Array.isArray(value)) {
          cookies.push(...(value as string[]));
        } else {
          cookies.push(value as string);
        }
        continue;
      }
      this.setHeader(key, value as string | string[]);
    }
    if (cookies.length) {
      this.setHeader('set-cookie', cookies);
    }

    return this;
  }

  appendHeader(
    name: string,
    value: number | string | ReadonlyArray<string> | OutgoingHttpHeader
  ): this {
    if (this._header) {
      throw new ERR_HTTP_HEADERS_SENT('append');
    }
    validateHeaderName(name);
    validateHeaderValue(name, value);

    const field = name.toLowerCase();
    const headers = this[kOutHeaders];
    if (headers === null || !headers[field]) {
      return this.setHeader(name, value);
    }

    // Prepare the field for appending, if required
    if (!Array.isArray(headers[field][1])) {
      headers[field][1] = [headers[field][1]];
    }

    const existingValues = headers[field][1];
    if (Array.isArray(value)) {
      for (let i = 0, length = value.length; i < length; i++) {
        existingValues.push(value[i] as string);
      }
    } else {
      existingValues.push(value);
    }

    return this;
  }

  getHeader(name: string): number | string | string[] | undefined {
    validateString(name, 'name');

    const headers = this[kOutHeaders];
    if (headers === null) {
      return;
    }

    const entry = headers[name.toLowerCase()];
    return entry?.[1] as string;
  }

  hasHeader(name: unknown): boolean {
    validateString(name, 'name');
    return Boolean(this[kOutHeaders]?.[name.toLowerCase()]);
  }

  removeHeader(name: string): void {
    validateString(name, 'name');

    if (this._header) {
      throw new ERR_HTTP_HEADERS_SENT('remove');
    }

    const key = name.toLowerCase();

    switch (key) {
      case 'connection':
        this._removedConnection = true;
        break;
      case 'content-length':
        this._removedContLen = true;
        break;
      case 'transfer-encoding':
        this._removedTE = true;
        break;
      case 'date':
        this.sendDate = false;
        break;
    }

    if (this[kOutHeaders] !== null) {
      // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
      delete this[kOutHeaders][key];
    }
  }

  // Returns an array of the names of the current outgoing headers.
  getHeaderNames(): string[] {
    return this[kOutHeaders] !== null ? Object.keys(this[kOutHeaders]) : [];
  }

  // Returns an array of the names of the current outgoing raw headers.
  getRawHeaderNames(): string[] {
    const headersMap = this[kOutHeaders];
    if (headersMap === null) return [];

    const values = Object.values(headersMap);
    const headers = Array.from<string>({ length: values.length });
    // Retain for(;;) loop for performance reasons
    // Refs: https://github.com/nodejs/node/pull/30958
    for (let i = 0, l = values.length; i < l; i++) {
      headers[i] = (values[i] as [string, string])[0];
    }

    return headers;
  }

  flushHeaders(): void {
    if (!this._header) {
      this._implicitHeader();
    }

    // Force-flush the headers.
    this._send('');
  }

  getHeaders(): OutgoingHttpHeaders {
    const headers = this[kOutHeaders];
    const ret: Record<string, string> = {};
    if (headers) {
      const keys = Object.keys(headers);
      // Retain for(;;) loop for performance reasons
      // Refs: https://github.com/nodejs/node/pull/30958
      for (let i = 0; i < keys.length; ++i) {
        const key = keys[i] as keyof typeof headers;
        const val = (headers[key] as [string, string])[1];
        ret[key] = val;
      }
    }
    return ret;
  }

  get headersSent(): boolean {
    return !!this._header;
  }

  override pipe<T extends NodeJS.WritableStream>(destination: T): T {
    this.emit('error', new ERR_STREAM_CANNOT_PIPE());
    return destination;
  }

  [EventEmitter.captureRejectionSymbol](error: Error): void {
    this.destroy(error);
  }

  _implicitHeader(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('_implicitHeader()');
  }

  _renderHeaders(): Record<string, string> {
    if (this._header) {
      throw new ERR_HTTP_HEADERS_SENT('render');
    }

    const headersMap = this[kOutHeaders];
    const headers: Record<string, string> = {};

    if (headersMap !== null) {
      const keys = Object.keys(headersMap);
      // Retain for(;;) loop for performance reasons
      // Refs: https://github.com/nodejs/node/pull/30958
      for (let i = 0; i < keys.length; i++) {
        const key = keys[i] as keyof typeof headersMap;
        headers[(headersMap[key] as [string, string])[0]] = (
          headersMap[key] as [string, string]
        )[1];
      }
    }
    return headers;
  }

  _send(
    data: string | Uint8Array,
    encoding?: BufferEncoding | WriteCallback | null,
    callback?: WriteCallback | null,
    byteLength?: number
  ): boolean {
    // This is a shameful hack to get the headers and first body chunk onto
    // the same packet. Future versions of Node are going to take care of
    // this at a lower level and in a more general way.
    if (!this._headerSent) {
      // If we ever hit this assertion, we have a bug with our implementation.
      ok(this._header);
      const header = this._header;
      if (
        typeof data === 'string' &&
        (encoding === 'utf8' || encoding === 'latin1' || !encoding)
      ) {
        data = header + data;
      } else {
        this.outputData.unshift({
          data: header,
          encoding: 'latin1',
          callback: null,
        });
        this.outputSize += header.length;
        this._onPendingData(header.length);
      }

      this._headerSent = true;

      // Difference from Node.js -
      // Parse headers here and trigger _headersSent
      this.writtenHeaderBytes = header.length;

      // Save written headers as object
      const [statusLine, ...headerLines] = this._header.split('\r\n') as [
        string,
        ...string[],
      ];

      const STATUS_LINE_REGEXP =
        /^HTTP\/1\.1 (?<statusCode>\d+) (?<statusMessage>.*)$/;
      const statusLineResult = STATUS_LINE_REGEXP.exec(statusLine);

      if (statusLineResult == null) {
        throw new Error(`Unexpected! Status line was ${statusLine}`);
      }

      const { statusCode: statusCodeText, statusMessage } =
        statusLineResult.groups ?? {};
      const headers = new Headers();

      for (const headerLine of headerLines) {
        if (headerLine !== '') {
          const pos = headerLine.indexOf(': ');
          // Skip the colon and the space on value
          headers.append(headerLine.slice(0, pos), headerLine.slice(pos + 2));
        }
      }
      this.emit('_headersSent', {
        statusCode: Number(statusCodeText as string),
        statusMessage,
        headers,
      } as HeadersSentEvent);
    }
    return this._writeRaw(data, encoding, callback, byteLength);
  }

  override write(
    chunk: string | Buffer | Uint8Array,
    encoding?: BufferEncoding | WriteCallback | null,
    callback?: WriteCallback
  ): boolean {
    if (typeof encoding === 'function') {
      callback = encoding;
      encoding = null;
    }

    const ret = this.#write(chunk, encoding, callback, false);
    if (!ret) {
      this[kNeedDrain] = true;
    }
    return ret;
  }

  override end(
    chunk?: string | Buffer | Uint8Array | WriteCallback | null,
    encoding?: BufferEncoding | WriteCallback | null,
    callback?: WriteCallback
  ): this {
    if (typeof chunk === 'function') {
      callback = chunk;
      chunk = null;
      encoding = null;
    } else if (typeof encoding === 'function') {
      callback = encoding;
      encoding = null;
    }

    if (chunk) {
      if (this.finished) {
        this.#onError(
          new ERR_STREAM_WRITE_AFTER_END(),
          typeof callback !== 'function' ? (): void => {} : callback
        );
        return this;
      }

      // Difference from Node.js -
      // In Node.js, if a socket exists, we would also call socket.cork() at this point.
      // For our implementation we do the same for the "written data buffer"
      this.#buffer?.cork();
      this.#write(chunk, encoding, null, true);
    } else if (this.finished) {
      if (typeof callback === 'function') {
        if (!this.writableFinished) {
          this.on('finish', callback);
        } else {
          callback(new ERR_STREAM_ALREADY_FINISHED('end'));
        }
      }
      return this;
    } else if (!this._header) {
      // Difference from Node.js -
      // In Node.js, if a socket exists, we would also call socket.cork() at this point.
      // For our implementation we do the same for the "written data buffer"
      this.#buffer?.cork();
      this._contentLength = 0;
      this._implicitHeader();
    }

    if (typeof callback === 'function') this.once('finish', callback);

    if (
      this.#checkStrictContentLength() &&
      this[kBytesWritten] !== this._contentLength
    ) {
      throw new ERR_HTTP_CONTENT_LENGTH_MISMATCH(
        this[kBytesWritten],
        this._contentLength ?? 0
      );
    }

    const finish = onFinish.bind(undefined, this);

    if (this._hasBody && this.chunkedEncoding) {
      // Difference from Node.js -
      // Chunked transfer encoding doesn't need to use the low-level protocol
      // (with each chunk preceded by its length)
      // So here we just send an empty chunk. Trailers are not supported

      // this._send("0\r\n" + this._trailer + "\r\n", "latin1", finish);
      this._send('', 'latin1', finish);
    } else if (!this._headerSent || this.writableLength || chunk) {
      this._send('', 'latin1', finish);
    } else {
      queueMicrotask(finish);
    }

    // Difference from Node.js -
    // In Node.js, if a socket exists, we would also call socket.uncork() at this point.
    // For our implementation we do the same for the "written data buffer"
    this.#buffer?.uncork();
    this[kCorked] = 1;
    this.uncork();

    this.finished = true;
    this._writableState.finished = true;
    this._writableState.corked = 1;

    // Difference from Node.js -
    // In Node.js, if a socket exists, and there is no pending output data,
    // we would also call this._finish() at this point.
    // For our implementation we do the same for the "written data buffer"
    if (this.outputData.length === 0 && this.#buffer != null) {
      this._finish();
    }

    return this;
  }

  _writeRaw(
    data: string | Uint8Array,
    encoding?: BufferEncoding | WriteCallback | null,
    callback?: WriteCallback | null,
    _size?: number
  ): boolean {
    if (this.destroyed) {
      return false;
    }

    // Difference from Node.js -
    // In Node.js, we would check for an underlying socket, and if that socket
    // exists and is already destroyed, simply return false.

    if (typeof encoding === 'function') {
      callback = encoding;
      encoding = null;
    }

    // Difference from Node.js -
    // In Node.js, we would check for an underlying socket, and if that socket
    // exists and is currently writable, it would flush any pending data to the socket and then
    // write the current chunk's data directly into the socket. Afterwards, it would return with the
    // value returned from socket.write().
    if (this.#buffer != null) {
      if (this.outputData.length) {
        this._flushOutput(this.#buffer);
      }
      return this.#buffer.write(data, encoding, callback);
    }

    this.outputData.push({ data, encoding, callback });
    this.outputSize += data.length;
    this._onPendingData(data.length);
    return this.outputSize < this[kHighWaterMark];
  }

  override destroy(err?: unknown, _cb?: (err?: unknown) => void): this {
    if (this.destroyed) {
      return this;
    }
    if (err != null) {
      this.emit('error', err);
    }
    this.destroyed = true;
    this[kErrored] = err as Error;

    return this;
  }

  // @ts-expect-error TS2611 Property accessor.
  get writableObjectMode(): boolean {
    return false;
  }

  // @ts-expect-error TS2611 Property accessor.
  get errored(): Error | null {
    return this[kErrored];
  }

  // @ts-expect-error TS2611 Property accessor.
  get closed(): boolean {
    return this._closed;
  }

  // @ts-expect-error TS2611 Property accessor.
  get writableEnded(): boolean {
    return this.finished;
  }

  // @ts-expect-error TS2611 Property accessor.
  get writableHighWaterMark(): number {
    return this.#buffer?.writableHighWaterMark ?? this[kHighWaterMark];
  }

  // @ts-expect-error TS2611 Property accessor.
  get writableObjectMode(): boolean {
    return false;
  }

  #checkStrictContentLength(): boolean {
    return (
      this.strictContentLength &&
      this._contentLength != null &&
      this._hasBody &&
      !this._removedContLen &&
      !this.chunkedEncoding &&
      !this.hasHeader('transfer-encoding')
    );
  }

  #onError(err: Error, callback: WriteCallback): void {
    if (this.destroyed) {
      return;
    }

    queueMicrotask(() => {
      emitErrorNt(this, err, callback);
    });
  }

  #write(
    this: OutgoingMessage,
    chunk: string | Buffer | Uint8Array | null,
    encoding: BufferEncoding | undefined | null,
    callback: WriteCallback | undefined | null,
    fromEnd: boolean
  ): boolean {
    if (typeof callback !== 'function') {
      callback = (): void => {};
    }

    if (chunk === null) {
      throw new ERR_STREAM_NULL_VALUES();
    } else if (typeof chunk !== 'string' && !isUint8Array(chunk)) {
      throw new ERR_INVALID_ARG_TYPE(
        'chunk',
        ['string', 'Buffer', 'Uint8Array'],
        chunk
      );
    }

    let err: Error | undefined = undefined;
    if (this.finished) {
      err = new ERR_STREAM_WRITE_AFTER_END();
    } else if (this.destroyed) {
      err = new ERR_STREAM_DESTROYED('write');
    }

    if (err) {
      if (!this.destroyed) {
        this.#onError(err, callback);
      } else {
        queueMicrotask(() => {
          callback(err);
        });
      }
      return false;
    }

    let len: number | undefined = undefined;

    if (this.strictContentLength) {
      len ??=
        typeof chunk === 'string'
          ? Buffer.byteLength(chunk, encoding ?? undefined)
          : chunk.byteLength;

      if (
        this.#checkStrictContentLength() &&
        (fromEnd
          ? this[kBytesWritten] + len !== this._contentLength
          : this[kBytesWritten] + len > (this._contentLength ?? 0))
      ) {
        throw new ERR_HTTP_CONTENT_LENGTH_MISMATCH(
          len + this[kBytesWritten],
          this._contentLength ?? 0
        );
      }

      this[kBytesWritten] += len;
    }

    if (!this._header) {
      if (fromEnd) {
        len ??=
          typeof chunk === 'string'
            ? Buffer.byteLength(chunk, encoding ?? undefined)
            : chunk.byteLength;
        this._contentLength = len;
      }
      this._implicitHeader();
    }

    if (!this._hasBody) {
      if (this[kRejectNonStandardBodyWrites]) {
        throw new ERR_HTTP_BODY_NOT_ALLOWED();
      } else {
        queueMicrotask(callback);
        return true;
      }
    }

    if (!fromEnd && this.#buffer != null && !this.#buffer.writableCorked) {
      this.#buffer.cork();
      queueMicrotask(() => {
        connectionCorkNT(this.#buffer as MessageBuffer);
      });
    }

    let ret;
    if (this.chunkedEncoding && chunk.length !== 0) {
      len ??=
        typeof chunk === 'string'
          ? Buffer.byteLength(chunk, encoding ?? undefined)
          : chunk.byteLength;
      if (this[kCorked] && this._headerSent) {
        this[kChunkedBuffer].push({ data: chunk, encoding, callback });
        this[kChunkedLength] += len;
        ret = this[kChunkedLength] < this[kHighWaterMark];
      } else {
        ret = this._send(chunk, encoding, callback, len);
      }
    } else {
      ret = this._send(chunk, encoding, callback, len);
    }

    return ret;
  }
}

function emitErrorNt(
  msg: OutgoingMessage,
  err: Error,
  callback: WriteCallback
): void {
  callback(err);
  if (typeof msg.emit === 'function' && !msg.destroyed) {
    msg.emit('error', err);
  }
}

function onFinish(outmsg: OutgoingMessage): void {
  outmsg.emit('finish');
}

function connectionCorkNT(buffer: MessageBuffer): void {
  buffer.uncork();
}

// isCookieField performs a case-insensitive comparison of a provided string
// against the word "cookie." As of V8 6.6 this is faster than handrolling or
// using a case-insensitive RegExp.
function isCookieField(s: string): boolean {
  return s.length === 6 && s.toLowerCase() === 'cookie';
}

function isContentDispositionField(s: string): boolean {
  return s.length === 19 && s.toLowerCase() === 'content-disposition';
}

function processHeader(
  self: OutgoingMessage,
  state: HeaderState,
  key: string,
  value: OutgoingHttpHeader,
  validate: boolean
): void {
  if (validate) {
    validateHeaderName(key);
  }

  // If key is content-disposition and there is content-length
  // encode the value in latin1
  // https://www.rfc-editor.org/rfc/rfc6266#section-4.3
  // Refs: https://github.com/nodejs/node/pull/46528
  if (isContentDispositionField(key) && self._contentLength) {
    // The value could be an array here
    if (Array.isArray(value)) {
      for (let i = 0; i < value.length; i++) {
        value[i] = String(Buffer.from(String(value[i]), 'latin1'));
      }
    } else {
      value = String(Buffer.from(String(value), 'latin1'));
    }
  }

  if (Array.isArray(value)) {
    if (
      (value.length < 2 || !isCookieField(key)) &&
      (!(kUniqueHeaders in self) ||
        !(self[kUniqueHeaders] as Set<string>).has(key.toLowerCase()))
    ) {
      // Retain for(;;) loop for performance reasons
      // Refs: https://github.com/nodejs/node/pull/30958
      for (let i = 0; i < value.length; i++) {
        storeHeader(self, state, key, value[i] as string, validate);
      }
      return;
    }
    value = value.join('; ');
  }
  storeHeader(self, state, key, String(value), validate);
}

function storeHeader(
  self: OutgoingMessage,
  state: HeaderState,
  key: string,
  value: string,
  validate: boolean
): void {
  if (validate) {
    validateHeaderValue(key, value);
  }
  state.header += key + ': ' + value + '\r\n';
  matchHeader(self, state, key, value);
}

function matchHeader(
  self: OutgoingMessage,
  state: HeaderState,
  field: string,
  value: string
): void {
  if (field.length < 4 || field.length > 17) return;
  field = field.toLowerCase();
  switch (field) {
    case 'connection':
      state.connection = true;
      self._removedConnection = false;
      if (RE_CONN_CLOSE.exec(value) !== null) self._last = true;
      else self.shouldKeepAlive = true;
      break;
    case 'transfer-encoding':
      state.te = true;
      self._removedTE = false;
      if (RE_TE_CHUNKED.exec(value) !== null) self.chunkedEncoding = true;
      break;
    case 'content-length':
      state.contLen = true;
      self._contentLength = +value;
      self._removedContLen = false;
      break;
    case 'date':
    case 'expect':
    case 'trailer':
      state[field] = true;
      break;
    case 'keep-alive':
      self._defaultKeepAlive = false;
      break;
  }
}
