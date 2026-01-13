// Copyright (c) 2024 Jeju Network
// Bun Internal Validators
// Licensed under the Apache 2.0 license

/**
 * Validation utilities for Bun runtime
 */

import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_OUT_OF_RANGE,
} from './errors'
import {
  isArrayBuffer,
  isBoolean,
  isFunction,
  isNumber,
  isObject,
  isString,
  isUint8Array,
} from './types'

export function validateString(
  value: unknown,
  name: string,
): asserts value is string {
  if (!isString(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'string', value)
  }
}

export function validateNumber(
  value: unknown,
  name: string,
): asserts value is number {
  if (!isNumber(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value)
  }
}

export function validateBoolean(
  value: unknown,
  name: string,
): asserts value is boolean {
  if (!isBoolean(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'boolean', value)
  }
}

export function validateFunction(
  value: unknown,
  name: string,
): asserts value is Function {
  if (!isFunction(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'function', value)
  }
}

export function validateObject(
  value: unknown,
  name: string,
): asserts value is object {
  if (!isObject(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'object', value)
  }
}

export function validateBuffer(
  value: unknown,
  name: string,
): asserts value is Uint8Array | ArrayBuffer {
  if (!isUint8Array(value) && !isArrayBuffer(value)) {
    throw new ERR_INVALID_ARG_TYPE(
      name,
      'Buffer, TypedArray, or ArrayBuffer',
      value,
    )
  }
}

export function validateInteger(
  value: unknown,
  name: string,
  min?: number,
  max?: number,
): asserts value is number {
  if (!isNumber(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value)
  }
  if (!Number.isInteger(value)) {
    throw new ERR_INVALID_ARG_VALUE(name, value, 'must be an integer')
  }
  if (min !== undefined && value < min) {
    throw new ERR_OUT_OF_RANGE(name, `>= ${min}`, value)
  }
  if (max !== undefined && value > max) {
    throw new ERR_OUT_OF_RANGE(name, `<= ${max}`, value)
  }
}

export function validateUint32(
  value: unknown,
  name: string,
): asserts value is number {
  validateInteger(value, name, 0, 4294967295)
}

export function validateInt32(
  value: unknown,
  name: string,
): asserts value is number {
  validateInteger(value, name, -2147483648, 2147483647)
}

export function validatePositiveInteger(
  value: unknown,
  name: string,
): asserts value is number {
  validateInteger(value, name, 1)
}

export function validateNonNegativeInteger(
  value: unknown,
  name: string,
): asserts value is number {
  validateInteger(value, name, 0)
}

export type BunEncoding =
  | 'ascii'
  | 'utf8'
  | 'utf-8'
  | 'utf16le'
  | 'ucs2'
  | 'ucs-2'
  | 'base64'
  | 'base64url'
  | 'latin1'
  | 'binary'
  | 'hex'

export function validateEncoding(
  value: unknown,
  name: string,
): asserts value is BunEncoding {
  if (!isString(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'string', value)
  }
  const valid: BunEncoding[] = [
    'ascii',
    'utf8',
    'utf-8',
    'utf16le',
    'ucs2',
    'ucs-2',
    'base64',
    'base64url',
    'latin1',
    'binary',
    'hex',
  ]
  if (!valid.includes(value.toLowerCase() as BunEncoding)) {
    throw new ERR_INVALID_ARG_VALUE(name, value, 'is not a valid encoding')
  }
}

export function validateCallback(
  value: unknown,
  name = 'callback',
): asserts value is (...args: unknown[]) => void {
  if (!isFunction(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'function', value)
  }
}

export function validateAbortSignal(
  value: unknown,
  name: string,
): asserts value is AbortSignal {
  if (value !== undefined && !(value instanceof AbortSignal)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'AbortSignal', value)
  }
}

export function validateOneOf<T>(
  value: T,
  name: string,
  oneOf: readonly T[],
): void {
  if (!oneOf.includes(value)) {
    throw new ERR_INVALID_ARG_VALUE(
      name,
      value,
      `must be one of: ${oneOf.map(String).join(', ')}`,
    )
  }
}
