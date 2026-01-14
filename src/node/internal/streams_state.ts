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

import { ERR_INVALID_ARG_VALUE } from 'node-internal:internal_errors'
import { validateInteger } from 'node-internal:validators'

let defaultHighWaterMarkBytes = 64 * 1024
let defaultHighWaterMarkObjectMode = 16

export type HighWaterMarkFromOptions = { highWaterMark?: number }

function highWaterMarkFrom(
  options: HighWaterMarkFromOptions,
  isDuplex: boolean,
  duplexKey: string,
): number | null {
  return options.highWaterMark != null
    ? options.highWaterMark
    : isDuplex
      ? // @ts-expect-error TS7053 Fix this soon.
        (options[duplexKey] as number)
      : null
}

export function getDefaultHighWaterMark(objectMode?: boolean): number {
  return objectMode ? defaultHighWaterMarkObjectMode : defaultHighWaterMarkBytes
}

export function setDefaultHighWaterMark(
  objectMode: boolean,
  value: unknown,
): void {
  validateInteger(value, 'value', 0)
  if (objectMode) {
    defaultHighWaterMarkObjectMode = value
  } else {
    defaultHighWaterMarkBytes = value
  }
}

export function getHighWaterMark(
  state: { objectMode?: boolean },
  options: HighWaterMarkFromOptions,
  duplexKey: string,
  isDuplex: boolean,
): number {
  const hwm = highWaterMarkFrom(options, isDuplex, duplexKey)
  if (hwm != null) {
    if (!Number.isInteger(hwm) || hwm < 0) {
      const name = isDuplex ? `options.${duplexKey}` : 'options.highWaterMark'
      throw new ERR_INVALID_ARG_VALUE(name, hwm)
    }
    return Math.floor(hwm)
  }

  // Default value
  return getDefaultHighWaterMark(state.objectMode)
}
