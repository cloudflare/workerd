// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { validateString } from 'node-internal:validators';
import {
  ERR_HTTP_HEADERS_SENT,
  ERR_INVALID_ARG_TYPE,
  ERR_STREAM_CANNOT_PIPE,
  ERR_STREAM_DESTROYED,
  ERR_METHOD_NOT_IMPLEMENTED,
} from 'node-internal:internal_errors';
import {
  validateHeaderName,
  validateHeaderValue,
} from 'node-internal:internal_http';
import { Writable } from 'node-internal:streams_writable';
import { EventEmitter } from 'node-internal:events';
import type {
  OutgoingMessage as _OutgoingMessage,
  OutgoingHttpHeaders,
} from 'node:http';

export const kUniqueHeaders = Symbol('kUniqueHeaders');
export const kHighWaterMark = Symbol('kHighWaterMark');
export const kNeedDrain = Symbol('kNeedDrain');
export const kOutHeaders = Symbol('kOutHeaders');
export const kErrored = Symbol('kErrored');

export function parseUniqueHeadersOption(
  headers?: string[] | null
): Set<string> | null {
  if (!Array.isArray(headers)) {
    return null;
  }

  const unique = new Set<string>();
  for (const header of headers) {
    unique.add(header.toLowerCase());
  }
  return unique;
}

export class OutgoingMessage extends Writable implements _OutgoingMessage {
  [kOutHeaders]: Record<string, { name: string; value: string | string[] }> =
    {};
  [kErrored]: Error | null = null;

  #header: unknown;

  override writable = true;
  override destroyed = false;
  finished = false;

  get _header(): unknown {
    return this.#header;
  }

  set _header(value: unknown) {
    this.#header = value;
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

    return this;
  }

  getHeader(name: unknown): string | string[] | undefined {
    validateString(name, 'name');
    return this[kOutHeaders][name.toLowerCase()]?.value;
  }

  hasHeader(name: unknown): boolean {
    validateString(name, 'name');
    return Boolean(this[kOutHeaders][name.toLowerCase()]);
  }

  removeHeader(name: string): void {
    validateString(name, 'name');

    if (this.headersSent) {
      throw new ERR_HTTP_HEADERS_SENT('remove');
    }

    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
    delete this[kOutHeaders][name.toLowerCase()];
  }

  // Returns an array of the names of the current outgoing headers.
  getHeaderNames(): string[] {
    return Object.keys(this[kOutHeaders]);
  }

  // Returns an array of the names of the current outgoing raw headers.
  getRawHeaderNames(): string[] {
    return Object.keys(this[kOutHeaders]).flatMap(
      (key) => this[kOutHeaders][key]?.name ?? []
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
    for (const [_key, entry] of Object.entries(this[kOutHeaders])) {
      headers[entry.name] = entry.value;
    }
    return headers;
  }

  get headersSent(): boolean {
    return this.#header != null;
  }

  // @ts-expect-error TS2416 Unnecessary type assertion
  pipe(): void {
    this.emit('error', new ERR_STREAM_CANNOT_PIPE());
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
    for (const [_key, entry] of Object.entries(this[kOutHeaders])) {
      headers[entry.name] = entry.value;
    }
    return headers;
  }

  _send(_val: unknown): void {
    // Unimplemented.
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
  get writableEnded(): boolean {
    // eslint-disable-next-line @typescript-eslint/no-deprecated
    return this.finished;
  }
}
