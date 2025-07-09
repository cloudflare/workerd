// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import { Buffer } from 'node-internal:internal_buffer';
import { normalizeEncoding } from 'node-internal:internal_utils';
import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_THIS,
  ERR_UNKNOWN_ENCODING,
} from 'node-internal:internal_errors';
import { default as bufferUtil } from 'node-internal:buffer';
import type { StringDecoder as _StringDecoder } from 'node:string_decoder';

const kIncompleteCharactersStart = 0;
const kIncompleteCharactersEnd = 4;
const kMissingBytes = 4;
const kBufferedBytes = 5;
const kEncoding = 6;
const kSize = 7;

// The order of this array should be in sync with i18n.h
// Encoding enum. Index of this array defines the uint8_t
// value of an encoding.
const encodings = [
  'ascii',
  'latin1',
  'utf8',
  'utf16le',
  'base64',
  'base64url',
  'hex',
];

const kNativeDecoder = Symbol('kNativeDecoder');

// @ts-expect-error TS2323 Cannot redeclare exported variable
export declare class StringDecoder extends _StringDecoder {
  [kNativeDecoder]?: Buffer & {
    [kBufferedBytes]?: number;
    [kMissingBytes]?: number;
    [kEncoding]?: number;
  };

  constructor(encoding?: string);

  encoding: string;
  readonly lastChar: Uint8Array;
  readonly lastNeed: number;
  readonly lastTotal: number;
  write(buf: ArrayBufferView | DataView | string): string;
  end(buf?: ArrayBufferView | DataView | string): string;
  text(buf: ArrayBufferView | DataView | string, offset?: number): string;
}

// @ts-expect-error TS2323 Cannot redeclare exported variable
export function StringDecoder(
  this: StringDecoder,
  encoding: string = 'utf8'
): StringDecoder {
  const normalizedEncoding = normalizeEncoding(encoding);
  if (normalizedEncoding === undefined) {
    throw new ERR_UNKNOWN_ENCODING(encoding);
  }
  this[kNativeDecoder] = Buffer.alloc(kSize);
  this[kNativeDecoder][kEncoding] = normalizedEncoding;
  this.encoding = encodings[normalizedEncoding] as string;
  return this;
}

function write(
  this: StringDecoder,
  buf: ArrayBufferView | DataView | string
): string {
  if (this[kNativeDecoder] === undefined) {
    throw new ERR_INVALID_THIS('StringDecoder');
  }
  if (typeof buf === 'string') {
    return buf;
  }
  if (!ArrayBuffer.isView(buf)) {
    throw new ERR_INVALID_ARG_TYPE(
      'buf',
      ['Buffer', 'TypedArray', 'DataView', 'string'],
      buf
    );
  }
  const buffer = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
  return bufferUtil.decode(buffer, this[kNativeDecoder]);
}

function end(
  this: StringDecoder,
  buf?: ArrayBufferView | DataView | string
): string {
  if (this[kNativeDecoder] === undefined) {
    throw new ERR_INVALID_THIS('StringDecoder');
  }
  let ret = '';
  if (buf !== undefined) {
    ret = this.write(buf);
  }
  if ((this[kNativeDecoder][kBufferedBytes] as number) > 0) {
    ret += bufferUtil.flush(this[kNativeDecoder]);
  }
  return ret;
}

function text(
  this: StringDecoder,
  buf: NodeJS.TypedArray | string,
  offset?: number
): string {
  if (this[kNativeDecoder] === undefined) {
    throw new ERR_INVALID_THIS('StringDecoder');
  }
  this[kNativeDecoder][kMissingBytes] = 0;
  this[kNativeDecoder][kBufferedBytes] = 0;
  return this.write(buf.slice(offset));
}

StringDecoder.prototype.write = write;
StringDecoder.prototype.end = end;
StringDecoder.prototype.text = text;

Object.defineProperties(StringDecoder.prototype, {
  lastChar: {
    enumerable: true,
    get(this: StringDecoder): Buffer {
      if (this[kNativeDecoder] === undefined) {
        throw new ERR_INVALID_THIS('StringDecoder');
      }
      return this[kNativeDecoder].subarray(
        kIncompleteCharactersStart,
        kIncompleteCharactersEnd
      ) as Buffer;
    },
  },
  lastNeed: {
    enumerable: true,
    get(this: StringDecoder): number {
      if (this[kNativeDecoder] === undefined) {
        throw new ERR_INVALID_THIS('StringDecoder');
      }
      return this[kNativeDecoder][kMissingBytes] as number;
    },
  },
  lastTotal: {
    enumerable: true,
    get(this: StringDecoder): number {
      if (this[kNativeDecoder] === undefined) {
        throw new ERR_INVALID_THIS('StringDecoder');
      }
      return (
        (this[kNativeDecoder][kBufferedBytes] ?? 0) +
        (this[kNativeDecoder][kMissingBytes] ?? 0)
      );
    },
  },
});

export default {
  StringDecoder,
};
