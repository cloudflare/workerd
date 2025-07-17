// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { validateString } from 'node-internal:validators';
import { Writable } from 'node-internal:streams_writable';
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
  ERR_HTTP_TRAILER_INVALID,
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
} from 'node:http';

type WriteCallback = (err?: Error) => void;
type OutputData = {
  data: string | Buffer | Uint8Array | null;
  encoding?: BufferEncoding | null | undefined;
  callback?: WriteCallback | null | undefined;
};
type WrittenDataBufferEntry = OutputData & {
  length: number;
  written: boolean;
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
  headers?: (string | string[])[] | undefined
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
  #data: WrittenDataBufferEntry[] = [];
  #onWrite?: (index: number, entry: WrittenDataBufferEntry) => void;
  #highWaterMark = 64 * 1024;

  constructor(onWrite: (index: number, entry: WrittenDataBufferEntry) => void) {
    this.#onWrite = onWrite;
  }

  write(
    data: WrittenDataBufferEntry['data'],
    encoding: WrittenDataBufferEntry['encoding'],
    callback: WrittenDataBufferEntry['callback']
  ) {
    this.#data.push({
      data,
      length: data?.length ?? 0,
      encoding,
      callback,
      written: false,
    });
    this._flush();

    return true;
  }

  cork() {
    this.#corked++;
  }

  uncork() {
    this.#corked--;
    this._flush();
  }

  _flush() {
    if (this.#corked <= 0) {
      for (const [index, entry] of this.#data.entries()) {
        if (!entry.written) {
          entry.written = true;
          this.#onWrite?.(index, entry);
          entry.callback?.call(undefined);
        }
      }
    }
  }

  get writableLength() {
    return this.#data.reduce<number>((acc, entry) => {
      return acc + (entry.written! && entry.length! ? entry.length : 0);
    }, 0);
  }

  get writableHighWaterMark() {
    return this.#highWaterMark;
  }

  get writableCorked() {
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
  [kOutHeaders]: Record<
    string,
    { name: string; value: string | string[] }
  > | null = null;
  [kErrored]: Error | null = null;
  [kCorked] = 0;
  [kChunkedBuffer]: OutputData[] = [];
  [kChunkedLength]: number = 0;
  [kNeedDrain] = false;
  [kRejectNonStandardBodyWrites]: boolean;
  [kHighWaterMark]: number;
  [kBytesWritten] = 0;

  readonly req: IncomingMessage;
  #header: unknown;
  #buffer: MessageBuffer = new MessageBuffer(this.#onDataWritten.bind(this));

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

  constructor(req: IncomingMessage, options?: OutgoingMessageOptions) {
    super();
    this.req = req;
    this[kHighWaterMark] = options?.highWaterMark ?? 64 * 1024;
    this[kRejectNonStandardBodyWrites] =
      options?.rejectNonStandardBodyWrites ?? false;
  }

  #onDataWritten(index: number, entry: WrittenDataBufferEntry): void {
    this.emit('_dataWritten', { index, entry });
  }

  override cork(): void {
    this[kCorked]++;
    this.#buffer.cork();
  }

  override uncork(): void {
    this[kCorked]--;
    this.#buffer.uncork();

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
    headers: Record<string, { name: string; value: string | string[] }>
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

    if (headers) {
      if (headers === this[kOutHeaders]) {
        for (const entry of Object.values(headers)) {
          processHeader(this, state, entry.name, entry.value, false);
        }
      } else if (Array.isArray(headers)) {
        if (headers.length && Array.isArray(headers[0])) {
          for (const entry of Object.values(headers)) {
            processHeader(this, state, entry.name, entry.value, true);
          }
        } else {
          throw new Error(
            'Invalid headers array. Headers array must be an array of arrays of strings'
          );
        }
      } else {
        for (const entry of Object.values(headers)) {
          processHeader(this, state, entry.name, entry.value, true);
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

    // Test non-chunked message does not have trailer header set,
    // message will be terminated by the first empty line after the
    // header fields, regardless of the header fields present in the
    // message, and thus cannot contain a message body or 'trailers'.
    if (this.chunkedEncoding !== true && state.trailer) {
      throw new ERR_HTTP_TRAILER_INVALID();
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

  _flushOutput(buffer: MessageBuffer) {
    while (this[kCorked]) {
      this[kCorked]--;
      buffer.cork();
    }

    const outputLength = this.outputData.length;
    if (outputLength <= 0) {
      return undefined;
    }

    const outputData = this.outputData;
    buffer.cork();
    let ret;
    // Retain for(;;) loop for performance reasons
    // Refs: https://github.com/nodejs/node/pull/30958
    for (let i = 0; i < outputLength; i++) {
      const { data, encoding, callback } = outputData[
        i
      ] as WrittenDataBufferEntry; // Avoid any potential ref to Buffer in new generation from old generation
      (outputData[i] as WrittenDataBufferEntry).data = null;
      ret = buffer.write(data ?? '', encoding, callback);
    }
    buffer.uncork();

    this.outputData = [];
    this._onPendingData(-this.outputSize);
    this.outputSize = 0;

    return ret;
  }

  _flush(): void {
    const ret = this._flushOutput(this.#buffer);

    if (this.finished) {
      this._finish();
    } else if (ret && this[kNeedDrain]) {
      this[kNeedDrain] = false;
      this.emit('drain');
    }
  }

  // @ts-expect-error TS2611
  get writableLength(): number {
    return this.outputSize + this[kChunkedLength] + this.#buffer.writableLength;
  }

  // @ts-expect-error TS2611
  get writableCorked(): number {
    return this[kCorked];
  }

  // @ts-expect-error TS2611
  get writableNeedDrain(): boolean {
    return !this.destroyed && !this.finished && this[kNeedDrain];
  }

  get header(): unknown {
    return this.#header;
  }

  setHeader(name: string, value: string | string[]): this {
    if (this.headersSent) {
      throw new ERR_HTTP_HEADERS_SENT('set');
    }
    validateHeaderName(name);
    validateHeaderValue(name, value);

    // Node.js uses an array, but we store it as an object.
    this[kOutHeaders] ??= {};
    this[kOutHeaders][name.toLowerCase()] = {
      name,
      value,
    };

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

  appendHeader(name: string, value: string | string[]): this {
    if (this.headersSent) {
      throw new ERR_HTTP_HEADERS_SENT('append');
    }

    validateHeaderName(name);
    validateHeaderValue(name, value);

    const field = name.toLowerCase();
    const headers = this[kOutHeaders];
    if (headers != null) {
      if (!headers[field]) {
        return this.setHeader(name, value);
      }

      // Prepare the field for appending, if required
      if (!Array.isArray(headers[field].value)) {
        headers[field].value = [headers[field].value];
      }

      const existingValues = headers[field].value;
      if (Array.isArray(value)) {
        existingValues.push(...value);
      } else {
        existingValues.push(value);
      }
    }

    return this;
  }

  getHeader(name: unknown): string | string[] | undefined {
    validateString(name, 'name');
    return this[kOutHeaders]?.[name.toLowerCase()]?.value;
  }

  hasHeader(name: unknown): boolean {
    validateString(name, 'name');
    return Boolean(this[kOutHeaders]?.[name.toLowerCase()]);
  }

  removeHeader(name: string): void {
    validateString(name, 'name');

    if (this.headersSent) {
      throw new ERR_HTTP_HEADERS_SENT('remove');
    }

    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
    delete this[kOutHeaders]?.[name.toLowerCase()];
  }

  // Returns an array of the names of the current outgoing headers.
  getHeaderNames(): string[] {
    return Object.keys(this[kOutHeaders] ?? []);
  }

  // Returns an array of the names of the current outgoing raw headers.
  getRawHeaderNames(): string[] {
    return Object.keys(this[kOutHeaders] ?? []).flatMap(
      (key) => this[kOutHeaders]?.[key]?.name ?? []
    );
  }

  flushHeaders(): void {
    if (!this._header) {
      this._implicitHeader();
    }

    // Force-flush the headers.
    this._send('');
  }

  getHeaders(): OutgoingHttpHeaders {
    const headers: Record<string, string | string[]> = {};
    for (const [_key, entry] of Object.entries(this[kOutHeaders] ?? [])) {
      headers[entry.name] = entry.value;
    }
    return headers;
  }

  get headersSent(): boolean {
    return this.#header != null;
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

  _renderHeaders(): Record<string, string | string[]> {
    if (this.headersSent) {
      throw new ERR_HTTP_HEADERS_SENT('render');
    }
    const headers: Record<string, string | string[]> = {};
    for (const [_key, entry] of Object.entries(this[kOutHeaders] ?? [])) {
      headers[entry.name] = entry.value;
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
      const header = this._header!;
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
      const [statusLine, ...headerLines] = (this._header as string).split(
        '\r\n'
      );

      const STATUS_LINE_REGEXP =
        /^HTTP\/1\.1 (?<statusCode>\d+) (?<statusMessage>.*)$/;
      const statusLineResult = STATUS_LINE_REGEXP.exec(statusLine as string);

      if (statusLineResult == null) {
        throw new Error('Unexpected! Status line was ' + statusLine);
      }

      const { statusCode: statusCodeText, statusMessage } =
        statusLineResult.groups ?? {};
      const statusCode = parseInt(statusCodeText as string, 10);
      const headers: [header: string, value: string][] = [];

      for (const headerLine of headerLines) {
        if (headerLine !== '') {
          const pos = headerLine.indexOf(': ');
          const k = headerLine.slice(0, pos);
          const v = headerLine.slice(pos + 2); // Skip the colon and the space
          headers.push([k, v]);
        }
      }
      this.emit('_headersSent', {
        statusCode,
        statusMessage,
        headers,
      });
    }
    return this._writeRaw(data, encoding, callback, byteLength);
  }

  override _write(
    _chunk: any, // eslint-disable-line @typescript-eslint/no-explicit-any
    _encoding: BufferEncoding,
    cb: (error?: Error | null) => void
  ): void {
    // The only reason for us to override this method is to increase the Node.js test coverage.
    // Otherwise, we don't implement _write yet.
    if (this.destroyed) {
      cb(new ERR_STREAM_DESTROYED('_write'));
      return;
    }

    throw new ERR_METHOD_NOT_IMPLEMENTED('_write');
  }

  override end(
    chunk?: string | Buffer | Uint8Array | WriteCallback | null,
    encoding?: BufferEncoding | WriteCallback | null,
    callback?: WriteCallback
  ) {
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
          typeof callback !== 'function' ? () => {} : callback
        );
        return this;
      }

      // Difference from Node.js -
      // In Node.js, if a socket exists, we would also call socket.cork() at this point.
      // For our implementation we do the same for the "written data buffer"
      if (this.#buffer != null) {
        this.#buffer.cork();
      }
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
      if (this.#buffer != null) {
        this.#buffer.cork();
      }
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
      setTimeout(finish, 0);
    }

    // Difference from Node.js -
    // In Node.js, if a socket exists, we would also call socket.uncork() at this point.
    // For our implementation we do the same for the "written data buffer"
    this.#buffer?.uncork();
    this[kCorked] = 1;
    this.uncork();

    this.finished = true;

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
      // There might be pending data in the this.output buffer.
      if (this.outputData.length) {
        this._flushOutput(this.#buffer);
      }
      // Directly write to the buffer.
      return this.#buffer.write(data, encoding, callback);
    }

    // Buffer, as long as we're not destroyed.
    this.outputData.push({ data, encoding, callback });
    this.outputSize += data.length;
    this._onPendingData(data.length);
    return this.outputSize < this[kHighWaterMark];
  }

  override destroy(err?: unknown, _cb?: (err?: unknown) => void): this {
    if (this.destroyed) {
      return this;
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
    // eslint-disable-next-line @typescript-eslint/no-deprecated
    return this.finished;
  }

  // @ts-expect-error TS2611 Property accessor.
  get writableHighWaterMark(): number {
    return this.#buffer?.writableHighWaterMark ?? this[kHighWaterMark];
  }

  // @ts-expect-error TS2611 Property accessor.
  get writableFinished(): boolean {
    return (
      this.finished &&
      this.outputSize === 0 &&
      this.#buffer.writableLength === 0
    );
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

    setTimeout(emitErrorNt, 0, this, err, callback);
  }

  #write(
    this: OutgoingMessage,
    chunk: string | Buffer | Uint8Array,
    encoding: BufferEncoding | undefined | null,
    callback: WriteCallback | undefined | null,
    fromEnd: boolean
  ) {
    if (typeof callback !== 'function') {
      callback = () => {};
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
        setTimeout(callback, 0, err);
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
        setTimeout(callback, 0);
        return true;
      }
    }

    // Difference from Node.js -
    // In Node.js, we would also check at this point if a socket exists and is not corked.
    // If so, we'd cork the socket and then queue up an 'uncork' for the next tick.
    // In our implementation we do the same for "written data buffer"
    if (!fromEnd && !this.#buffer?.writableCorked) {
      this.#buffer.cork();
      setTimeout(connectionCorkNT, 0, this.#buffer);
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

function connectionCorkNT(buffer: MessageBuffer) {
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
  value: string | string[] | Buffer | Buffer[],
  validate: boolean = false
) {
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
        value[i] = Buffer.from(value[i] as string, 'latin1');
      }
    } else {
      value = Buffer.from(value as string, 'latin1');
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
      for (let i = 0; i < value.length; i++)
        storeHeader(self, state, key, value[i] as string, validate);
      return;
    }
    value = value.join('; ');
  }
  storeHeader(self, state, key, value as string, validate);
}

function storeHeader(
  self: OutgoingMessage,
  state: HeaderState,
  key: string,
  value: string,
  validate: boolean
) {
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
) {
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
