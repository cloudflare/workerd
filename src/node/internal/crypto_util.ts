// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Deno and Node.js:
// Copyright 2018-2022 the Deno authors. All rights reserved. MIT license.
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
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

'use strict';

import {
  Buffer,
} from 'node-internal:internal_buffer';

import {
    isAnyArrayBuffer,
    isArrayBufferView,
} from 'node-internal:internal_types';

import {
    ERR_INVALID_ARG_TYPE,
  } from 'node-internal:internal_errors';

export function getArrayBufferOrView(buffer: Buffer | ArrayBuffer | ArrayBufferView | string, name: string, encoding?: string): Buffer | ArrayBuffer | ArrayBufferView {
  if (isAnyArrayBuffer(buffer))
    return buffer as ArrayBuffer;
  if (typeof buffer === 'string') {
    if (encoding === undefined || encoding === 'buffer')
      encoding = 'utf8';
    return Buffer.from(buffer, encoding);
  }
  if (!isArrayBufferView(buffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      name,
      [
        'string',
        'ArrayBuffer',
        'Buffer',
        'TypedArray',
        'DataView',
      ],
      buffer,
    );
  }
  return buffer;
}
