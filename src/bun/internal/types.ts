// Copyright (c) 2024 Jeju Network
// Bun Internal Type Utilities
// Licensed under the Apache 2.0 license

/**
 * Type checking utilities for Bun runtime
 */

export function isString(value: unknown): value is string {
  return typeof value === 'string'
}

export function isNumber(value: unknown): value is number {
  return typeof value === 'number'
}

export function isBigInt(value: unknown): value is bigint {
  return typeof value === 'bigint'
}

export function isBoolean(value: unknown): value is boolean {
  return typeof value === 'boolean'
}

export function isSymbol(value: unknown): value is symbol {
  return typeof value === 'symbol'
}

export function isUndefined(value: unknown): value is undefined {
  return value === undefined
}

export function isNull(value: unknown): value is null {
  return value === null
}

export function isNullOrUndefined(value: unknown): value is null | undefined {
  return value === null || value === undefined
}

export function isObject(value: unknown): value is object {
  return value !== null && typeof value === 'object'
}

export function isFunction(value: unknown): value is Function {
  return typeof value === 'function'
}

export function isArray(value: unknown): value is unknown[] {
  return Array.isArray(value)
}

export function isArrayBuffer(value: unknown): value is ArrayBuffer {
  return value instanceof ArrayBuffer
}

export function isSharedArrayBuffer(
  value: unknown,
): value is SharedArrayBuffer {
  return (
    typeof SharedArrayBuffer !== 'undefined' &&
    value instanceof SharedArrayBuffer
  )
}

export function isAnyArrayBuffer(
  value: unknown,
): value is ArrayBuffer | SharedArrayBuffer {
  return isArrayBuffer(value) || isSharedArrayBuffer(value)
}

export function isTypedArray(value: unknown): value is ArrayBufferView {
  return ArrayBuffer.isView(value) && !(value instanceof DataView)
}

export function isUint8Array(value: unknown): value is Uint8Array {
  return value instanceof Uint8Array
}

export function isUint16Array(value: unknown): value is Uint16Array {
  return value instanceof Uint16Array
}

export function isUint32Array(value: unknown): value is Uint32Array {
  return value instanceof Uint32Array
}

export function isInt8Array(value: unknown): value is Int8Array {
  return value instanceof Int8Array
}

export function isInt16Array(value: unknown): value is Int16Array {
  return value instanceof Int16Array
}

export function isInt32Array(value: unknown): value is Int32Array {
  return value instanceof Int32Array
}

export function isFloat32Array(value: unknown): value is Float32Array {
  return value instanceof Float32Array
}

export function isFloat64Array(value: unknown): value is Float64Array {
  return value instanceof Float64Array
}

export function isBigInt64Array(value: unknown): value is BigInt64Array {
  return value instanceof BigInt64Array
}

export function isBigUint64Array(value: unknown): value is BigUint64Array {
  return value instanceof BigUint64Array
}

export function isDataView(value: unknown): value is DataView {
  return value instanceof DataView
}

export function isDate(value: unknown): value is Date {
  return value instanceof Date
}

export function isRegExp(value: unknown): value is RegExp {
  return value instanceof RegExp
}

export function isError(value: unknown): value is Error {
  return value instanceof Error
}

export function isMap<K = unknown, V = unknown>(
  value: unknown,
): value is Map<K, V> {
  return value instanceof Map
}

export function isSet<T = unknown>(value: unknown): value is Set<T> {
  return value instanceof Set
}

export function isWeakMap<K extends WeakKey = WeakKey, V = unknown>(
  value: unknown,
): value is WeakMap<K, V> {
  return value instanceof WeakMap
}

export function isWeakSet<T extends WeakKey = WeakKey>(
  value: unknown,
): value is WeakSet<T> {
  return value instanceof WeakSet
}

export function isPromise<T = unknown>(value: unknown): value is Promise<T> {
  return value instanceof Promise
}

export function isAsyncFunction(
  value: unknown,
): value is (...args: unknown[]) => Promise<unknown> {
  return isFunction(value) && value.constructor.name === 'AsyncFunction'
}

export function isGeneratorFunction(
  value: unknown,
): value is GeneratorFunction {
  return isFunction(value) && value.constructor.name === 'GeneratorFunction'
}

export function isAsyncGeneratorFunction(
  value: unknown,
): value is AsyncGeneratorFunction {
  return (
    isFunction(value) && value.constructor.name === 'AsyncGeneratorFunction'
  )
}

export function isIterator(value: unknown): value is Iterator<unknown> {
  return isObject(value) && isFunction((value as Record<string, unknown>).next)
}

export function isIterable(value: unknown): value is Iterable<unknown> {
  return (
    value !== null &&
    value !== undefined &&
    isFunction((value as Record<symbol, unknown>)[Symbol.iterator])
  )
}

export function isAsyncIterable(
  value: unknown,
): value is AsyncIterable<unknown> {
  return (
    value !== null &&
    value !== undefined &&
    isFunction((value as Record<symbol, unknown>)[Symbol.asyncIterator])
  )
}

export function isPrimitive(
  value: unknown,
): value is string | number | bigint | boolean | symbol | null | undefined {
  return (
    value === null || (typeof value !== 'object' && typeof value !== 'function')
  )
}

export function getTypeName(value: unknown): string {
  if (value === null) return 'null'
  if (value === undefined) return 'undefined'

  const type = typeof value
  if (type !== 'object') return type

  if (Array.isArray(value)) return 'array'

  const tag = Object.prototype.toString.call(value)
  return tag.slice(8, -1).toLowerCase()
}
