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

// Implementation of Node.js util.types, internal/util/types, and
// polyfill of internalBinding('types'). Based on the Deno polyfill.

/* eslint-disable */

const _toString = Object.prototype.toString;

const _isObjectLike = (value: unknown): boolean =>
  value !== null && typeof value === "object";

const _isFunctionLike = (value: unknown): boolean =>
  value !== null && typeof value === "function";

export function isAnyArrayBuffer(value: unknown): boolean {
  return (
    _isObjectLike(value) &&
    (_toString.call(value) === "[object ArrayBuffer]" ||
      _toString.call(value) === "[object SharedArrayBuffer]")
  );
}

export function isArgumentsObject(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Arguments]";
}

export function isArrayBuffer(value: unknown): boolean {
  return (
    _isObjectLike(value) && _toString.call(value) === "[object ArrayBuffer]"
  );
}

export function isAsyncFunction(value: unknown): boolean {
  return (
    _isFunctionLike(value) && _toString.call(value) === "[object AsyncFunction]"
  );
}

export function isBooleanObject(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Boolean]";
}

export function isBoxedPrimitive(value: unknown): boolean {
  return (
    isBooleanObject(value) ||
    isStringObject(value) ||
    isNumberObject(value) ||
    isSymbolObject(value) ||
    isBigIntObject(value)
  );
}

export function isDataView(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object DataView]";
}

export function isDate(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Date]";
}

export function isGeneratorFunction(value: unknown): boolean {
  return (
    _isFunctionLike(value) &&
    _toString.call(value) === "[object GeneratorFunction]"
  );
}

export function isGeneratorObject(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Generator]";
}

export function isMap(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Map]";
}

export function isMapIterator(value: unknown): boolean {
  return (
    _isObjectLike(value) && _toString.call(value) === "[object Map Iterator]"
  );
}

export function isModuleNamespaceObject(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Module]";
}

export function isNativeError(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Error]";
}

export function isNumberObject(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Number]";
}

export function isBigIntObject(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object BigInt]";
}

export function isPromise(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Promise]";
}

export function isRegExp(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object RegExp]";
}

export function isSet(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Set]";
}

export function isSetIterator(value: unknown): boolean {
  return (
    _isObjectLike(value) && _toString.call(value) === "[object Set Iterator]"
  );
}

export function isSharedArrayBuffer(value: unknown): boolean {
  return (
    _isObjectLike(value) &&
    _toString.call(value) === "[object SharedArrayBuffer]"
  );
}

export function isStringObject(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object String]";
}

export function isSymbolObject(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object Symbol]";
}

export function isWeakMap(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object WeakMap]";
}

export function isWeakSet(value: unknown): boolean {
  return _isObjectLike(value) && _toString.call(value) === "[object WeakSet]";
}

export function isCryptoKey(value: unknown): boolean {
  return value instanceof CryptoKey;
}

// @ts-ignore
export function isKeyObject(value: unknown): boolean {
  // TODO(nodecompat): We currently do not implement KeyObject
  return false;
}

// https://tc39.es/ecma262/#sec-get-%typedarray%.prototype-@@tostringtag
const _getTypedArrayToStringTag = Object.getOwnPropertyDescriptor(
  Object.getPrototypeOf(Uint8Array).prototype,
  Symbol.toStringTag,
)!.get!;

export function isArrayBufferView(value: unknown): boolean {
  return ArrayBuffer.isView(value);
}

export function isBigInt64Array(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) === "BigInt64Array";
}

export function isBigUint64Array(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) === "BigUint64Array";
}

export function isFloat32Array(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) === "Float32Array";
}

export function isFloat64Array(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) === "Float64Array";
}

export function isInt8Array(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) === "Int8Array";
}

export function isInt16Array(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) === "Int16Array";
}

export function isInt32Array(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) === "Int32Array";
}

export function isTypedArray(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) !== undefined;
}

export function isUint8Array(value: unknown): value is Uint8Array {
  return _getTypedArrayToStringTag.call(value) === "Uint8Array";
}

export function isUint8ClampedArray(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) === "Uint8ClampedArray";
}

export function isUint16Array(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) === "Uint16Array";
}

export function isUint32Array(value: unknown): boolean {
  return _getTypedArrayToStringTag.call(value) === "Uint32Array";
}

export default {
  isAsyncFunction,
  isGeneratorFunction,
  isGeneratorObject,
  isAnyArrayBuffer,
  isArrayBuffer,
  isArgumentsObject,
  isBoxedPrimitive,
  isDataView,
  isMap,
  isMapIterator,
  isModuleNamespaceObject,
  isNativeError,
  isPromise,
  isSet,
  isSetIterator,
  isSharedArrayBuffer,
  isWeakMap,
  isWeakSet,
  isRegExp,
  isDate,
  isStringObject,
  isSymbolObject,
  isNumberObject,
  isBooleanObject,
  isBigIntObject,
  isCryptoKey,
  isKeyObject,
  isArrayBufferView,
  isBigInt64Array,
  isBigUint64Array,
  isFloat32Array,
  isFloat64Array,
  isInt8Array,
  isInt16Array,
  isInt32Array,
  isTypedArray,
  isUint8Array,
  isUint8ClampedArray,
  isUint16Array,
  isUint32Array,
  // isExternal,
  // isProxy.
  // TODO(soon): isExternal and isProxy can currently only be implemented as C++
  // functions as JS gives us no mechanism for detecting these.
};
