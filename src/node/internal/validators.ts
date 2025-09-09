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
import { isArrayBufferView } from 'node-internal:internal_types';
import { normalizeEncoding } from 'node-internal:internal_utils';
import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_SOCKET_BAD_PORT,
  ERR_OUT_OF_RANGE,
  ERR_INVALID_THIS,
} from 'node-internal:internal_errors';
import { default as bufferUtil } from 'node-internal:buffer';

// TODO(someday): Not current implementing parseFileMode

export function isInt32(value: unknown): value is number {
  if (typeof value !== 'number') return false;
  return value === (value | 0);
}
export function isUint32(value: unknown): value is number {
  // @ts-expect-error Due to value being unknown
  return value === value >>> 0;
}

export function validateBuffer(
  buffer: unknown,
  name = 'buffer'
): asserts buffer is Buffer {
  if (!isArrayBufferView(buffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      name,
      ['Buffer', 'TypedArray', 'DataView'],
      buffer
    );
  }
}

export function validateInteger(
  value: unknown,
  name: string,
  min = Number.MIN_SAFE_INTEGER,
  max = Number.MAX_SAFE_INTEGER
): asserts value is number {
  if (typeof value !== 'number') {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
  }
  if (!Number.isInteger(value)) {
    throw new ERR_OUT_OF_RANGE(name, 'an integer', value);
  }
  if (value < min || value > max) {
    throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
  }
}

export function validateObject(
  value: unknown,
  name: string,
  options: number = kValidateObjectNone
): asserts value is Record<string, unknown> {
  if (options === kValidateObjectNone) {
    if (value === null || Array.isArray(value)) {
      throw new ERR_INVALID_ARG_TYPE(name, 'Object', value);
    }

    if (typeof value !== 'object') {
      throw new ERR_INVALID_ARG_TYPE(name, 'Object', value);
    }
  } else {
    const throwOnNullable = (kValidateObjectAllowNullable & options) === 0;

    if (throwOnNullable && value === null) {
      throw new ERR_INVALID_ARG_TYPE(name, 'Object', value);
    }

    const throwOnArray = (kValidateObjectAllowArray & options) === 0;

    if (throwOnArray && Array.isArray(value)) {
      throw new ERR_INVALID_ARG_TYPE(name, 'Object', value);
    }

    const throwOnFunction = (kValidateObjectAllowFunction & options) === 0;
    const typeofValue = typeof value;

    if (
      typeofValue !== 'object' &&
      (throwOnFunction || typeofValue !== 'function')
    ) {
      throw new ERR_INVALID_ARG_TYPE(name, 'Object', value);
    }
  }
}

export function validateInt32(
  value: unknown,
  name: string,
  min = -2147483648,
  max = 2147483647
): asserts value is number {
  if (!isInt32(value)) {
    if (typeof value !== 'number') {
      throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
    }

    if (!Number.isInteger(value)) {
      throw new ERR_OUT_OF_RANGE(name, 'an integer', value);
    }

    throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
  }

  if (value < min || value > max) {
    throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
  }
}

export function validateUint32(
  value: unknown,
  name: string,
  positive?: boolean
): asserts value is number {
  if (!isUint32(value)) {
    if (typeof value !== 'number') {
      throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
    }
    if (!Number.isInteger(value)) {
      throw new ERR_OUT_OF_RANGE(name, 'an integer', value);
    }
    const min = positive ? 1 : 0;
    // 2 ** 32 === 4294967296
    throw new ERR_OUT_OF_RANGE(name, `>= ${min} && < 4294967296`, value);
  }
  if (positive && value === 0) {
    throw new ERR_OUT_OF_RANGE(name, '>= 1 && < 4294967296', value);
  }
}

export function validateString(
  value: unknown,
  name: string
): asserts value is string {
  if (typeof value !== 'string') {
    throw new ERR_INVALID_ARG_TYPE(name, 'string', value);
  }
}

export function validateStringArray(
  value: unknown,
  name: string
): asserts value is string[] {
  validateArray(value, name);
  for (let i = 0; i < value.length; ++i) {
    // Don't use validateString here for performance reasons, as
    // we would generate intermediate strings for the name.
    if (typeof value[i] !== 'string') {
      throw new ERR_INVALID_ARG_TYPE(`${name}[${i}]`, 'string', value[i]);
    }
  }
}

export function validateNumber(
  value: unknown,
  name: string,
  min?: number,
  max?: number
): asserts value is number {
  if (typeof value !== 'number') {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
  }

  if (
    (min != null && value < min) ||
    (max != null && value > max) ||
    ((min != null || max != null) && Number.isNaN(value))
  ) {
    throw new ERR_OUT_OF_RANGE(
      name,
      `${min != null ? `>= ${min}` : ''}${min != null && max != null ? ' && ' : ''}${max != null ? `<= ${max}` : ''}`,
      value
    );
  }
}

export function validateBoolean(
  value: unknown,
  name: string
): asserts value is boolean {
  if (typeof value !== 'boolean') {
    throw new ERR_INVALID_ARG_TYPE(name, 'boolean', value);
  }
}

export function validateOneOf(
  value: unknown,
  name: string,
  oneOf: unknown[]
): void {
  if (!Array.prototype.includes.call(oneOf, value)) {
    const allowed = Array.prototype.join.call(
      Array.prototype.map.call(oneOf, (v) =>
        typeof v === 'string' ? `'${v}'` : String(v)
      ),
      ', '
    );
    const reason = 'must be one of: ' + allowed;

    throw new ERR_INVALID_ARG_VALUE(name, value, reason);
  }
}

export function validateEncoding(
  data: unknown,
  encoding: string
): asserts data is string {
  const normalizedEncoding = normalizeEncoding(encoding);
  const length = (data as any).length; // eslint-disable-line

  if (normalizedEncoding === bufferUtil.HEX && length % 2 !== 0) {
    throw new ERR_INVALID_ARG_VALUE(
      'encoding',
      encoding,
      `is invalid for data of length ${length}`
    );
  }
}

export function validateAbortSignal(
  signal: unknown,
  name: string
): asserts signal is AbortSignal {
  if (
    signal !== undefined &&
    (signal === null || typeof signal !== 'object' || !('aborted' in signal))
  ) {
    throw new ERR_INVALID_ARG_TYPE(name, 'AbortSignal', signal);
  }
}

export function validateFunction(
  value: unknown,
  name: string
): asserts value is (...args: unknown[]) => unknown {
  if (typeof value !== 'function') {
    throw new ERR_INVALID_ARG_TYPE(name, 'Function', value);
  }
}

export function validateArray(
  value: unknown,
  name: string,
  minLength = 0
): asserts value is unknown[] {
  if (!Array.isArray(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'Array', value);
  }
  if (value.length < minLength) {
    const reason = `must be longer than ${minLength}`;
    throw new ERR_INVALID_ARG_VALUE(name, value, reason);
  }
}

// 1. Returns false for undefined and NaN
// 2. Returns true for finite numbers
// 3. Throws ERR_INVALID_ARG_TYPE for non-numbers
// 4. Throws ERR_OUT_OF_RANGE for infinite numbers
export function checkFiniteNumber(
  number: unknown,
  name: string
): number is number {
  // Common case
  if (number === undefined) {
    return false;
  }

  if (Number.isFinite(number)) {
    return true; // Is a valid number
  }

  if (Number.isNaN(number)) {
    return false;
  }

  validateNumber(number, name);

  // Infinite numbers
  throw new ERR_OUT_OF_RANGE(name, 'a finite number', number);
}

// 1. Returns def for number when it's undefined or NaN
// 2. Returns number for finite numbers >= lower and <= upper
// 3. Throws ERR_INVALID_ARG_TYPE for non-numbers
// 4. Throws ERR_OUT_OF_RANGE for infinite numbers or numbers > upper or < lower
export function checkRangesOrGetDefault(
  number: unknown,
  name: string,
  lower: number,
  upper: number,
  def: number
): number;
export function checkRangesOrGetDefault(
  number: unknown,
  name: string,
  lower: number,
  upper: number,
  def?: number
): number | undefined;
export function checkRangesOrGetDefault(
  number: unknown,
  name: string,
  lower: number,
  upper: number,
  def: number | undefined = undefined
): number | undefined {
  if (!checkFiniteNumber(number, name)) {
    return def;
  }
  if (number < lower || number > upper) {
    throw new ERR_OUT_OF_RANGE(name, `>= ${lower} and <= ${upper}`, number);
  }
  return number;
}

export function validatePort(
  port: unknown,
  name = 'Port',
  allowZero = true
): number {
  if (
    (typeof port !== 'number' && typeof port !== 'string') ||
    (typeof port === 'string' && port.trim().length === 0) ||
    +port !== +port >>> 0 ||
    +port > 0xffff ||
    (port === 0 && !allowZero)
  ) {
    throw new ERR_SOCKET_BAD_PORT(name, port, allowZero);
  }
  return +port | 0;
}

const octalReg = /^[0-7]+$/;
const modeDesc = 'must be a 32-bit unsigned integer or an octal string';

export function parseFileMode(
  value: unknown,
  name: string,
  def?: number
): number {
  if (def != null) {
    value ??= def;
  }
  if (typeof value === 'string') {
    if (!octalReg.test(value)) {
      throw new ERR_INVALID_ARG_VALUE(name, value, modeDesc);
    }
    value = Number.parseInt(value, 8);
  }

  validateUint32(value, name);
  return value;
}

export function validateThisInternalField(
  object: object | null,
  fieldKey: string | symbol,
  className: string
): void {
  if (
    typeof object !== 'object' ||
    object === null ||
    !Object.prototype.hasOwnProperty.call(object, fieldKey)
  ) {
    throw new ERR_INVALID_THIS(className);
  }
}

export const kValidateObjectNone = 0;
export const kValidateObjectAllowNullable = 1 << 0;
export const kValidateObjectAllowArray = 1 << 1;
export const kValidateObjectAllowFunction = 1 << 2;
export const kValidateObjectAllowObjects =
  kValidateObjectAllowArray | kValidateObjectAllowFunction;
export const kValidateObjectAllowObjectsAndNull =
  kValidateObjectAllowNullable |
  kValidateObjectAllowArray |
  kValidateObjectAllowFunction;

export default {
  isInt32,
  isUint32,
  validateAbortSignal,
  validateArray,
  validateBoolean,
  validateBuffer,
  validateFunction,
  validateInt32,
  validateInteger,
  validateNumber,
  validateObject,
  validateOneOf,
  validateString,
  validateUint32,
  validatePort,
  validateThisInternalField,

  // Zlib specific
  checkFiniteNumber,
  checkRangesOrGetDefault,

  // Filesystem specific
  parseFileMode,
  kValidateObjectNone,
  kValidateObjectAllowNullable,
  kValidateObjectAllowArray,
  kValidateObjectAllowFunction,
  kValidateObjectAllowObjects,
  kValidateObjectAllowObjectsAndNull,
};
