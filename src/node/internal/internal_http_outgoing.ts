// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { validateString } from 'node-internal:validators';
import {
  ERR_HTTP_HEADERS_SENT,
  ERR_INVALID_ARG_TYPE,
  ERR_STREAM_CANNOT_PIPE,
  ERR_METHOD_NOT_IMPLEMENTED,
} from 'node-internal:internal_errors';
import {
  validateHeaderName,
  validateHeaderValue,
} from 'node-internal:internal_http';
import { Writable } from 'node-internal:streams_writable';
import { EventEmitter } from 'node-internal:events';

export const kUniqueHeaders = Symbol('kUniqueHeaders');
export const kHighWaterMark = Symbol('kHighWaterMark');
export const kNeedDrain = Symbol('kNeedDrain');
export const kOutHeaders = Symbol('kOutHeaders');
export const kHeadersSent = Symbol('kHeadersSent');

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

export class OutgoingMessage extends Writable {
  public [kHeadersSent]: boolean = false;
  public [kOutHeaders]: Record<string, Record<string, string | string[]>> = {};

  public setHeader(name: string, value: string | string[]): this {
    if (this[kHeadersSent]) {
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

  public setHeaders(headers: HeadersInit): this {
    if (this[kHeadersSent]) {
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

    // @ts-expect-error TS2488 headers is missing iterator type.
    for (const { 0: key, 1: value } of headers) {
      if (key === 'set-cookie') {
        if (Array.isArray(value)) {
          cookies.push(...(value as string[]));
        } else {
          cookies.push(value as string);
        }
        continue;
      }
      this.setHeader(key as string, value as string | string[]);
    }
    if (cookies.length) {
      this.setHeader('set-cookie', cookies);
    }

    return this;
  }

  public appendHeader(name: string, value: string | string[]): this {
    if (this[kHeadersSent]) {
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
      headers[field].value = [headers[field].value as string];
    }

    const existingValues = headers[field].value;
    if (Array.isArray(value)) {
      existingValues.push(...value);
    } else {
      existingValues.push(value);
    }

    return this;
  }

  public getHeader(name: unknown): string | string[] | undefined {
    validateString(name, 'name');
    return this[kOutHeaders][name.toLowerCase()]?.value;
  }

  public hasHeader(name: unknown): boolean {
    validateString(name, 'name');
    return Boolean(this[kOutHeaders][name.toLowerCase()]);
  }

  public removeHeader(name: string): void {
    validateString(name, 'name');

    if (this[kHeadersSent]) {
      throw new ERR_HTTP_HEADERS_SENT('remove');
    }

    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
    delete this[kOutHeaders][name.toLowerCase()];
  }

  // Returns an array of the names of the current outgoing headers.
  public getHeaderNames(): string[] {
    return Object.keys(this[kOutHeaders]);
  }

  // Returns an array of the names of the current outgoing raw headers.
  public getRawHeaderNames(): string[] {
    return Object.keys(this[kOutHeaders]).flatMap(
      (key) => this[kOutHeaders][key]?.name ?? []
    );
  }

  public flushHeaders(): void {
    // Not implemented
  }

  public get headersSent(): boolean {
    return this[kHeadersSent];
  }

  // @ts-expect-error TS2416 Unnecessary type assertion
  public pipe(): void {
    this.emit('error', new ERR_STREAM_CANNOT_PIPE());
  }

  public [EventEmitter.captureRejectionSymbol](error: Error): void {
    this.destroy(error);
  }

  public _implicitHeader(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('_implicitHeader()');
  }
}
