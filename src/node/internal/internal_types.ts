// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
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

import internal from "node-internal:util";

export function isCryptoKey(value: unknown): boolean {
  return value instanceof CryptoKey;
}

export function isKeyObject(_value: unknown): boolean {
  // TODO(nodecompat): We currently do not implement KeyObject
  return false;
}

export const isAsyncFunction = internal.isAsyncFunction.bind(internal);
export const isGeneratorFunction = internal.isGeneratorFunction.bind(internal);
export const isGeneratorObject = internal.isGeneratorObject.bind(internal);
export const isAnyArrayBuffer = internal.isAnyArrayBuffer.bind(internal);
export const isArrayBuffer = internal.isArrayBuffer.bind(internal);
export const isArgumentsObject = internal.isArgumentsObject.bind(internal);
export const isBoxedPrimitive = internal.isBoxedPrimitive.bind(internal);
export const isDataView = internal.isDataView.bind(internal);
export const isMap = internal.isMap.bind(internal);
export const isMapIterator = internal.isMapIterator.bind(internal);
export const isModuleNamespaceObject = internal.isModuleNamespaceObject.bind(internal);
export const isNativeError = internal.isNativeError.bind(internal);
export const isPromise = internal.isPromise.bind(internal);
export const isProxy = internal.isProxy.bind(internal);
export const isSet = internal.isSet.bind(internal);
export const isSetIterator = internal.isSetIterator.bind(internal);
export const isSharedArrayBuffer = internal.isSharedArrayBuffer.bind(internal);
export const isWeakMap = internal.isWeakMap.bind(internal);
export const isWeakSet = internal.isWeakSet.bind(internal);
export const isRegExp = internal.isRegExp.bind(internal);
export const isDate = internal.isDate.bind(internal);
export const isStringObject = internal.isStringObject.bind(internal);
export const isSymbolObject = internal.isSymbolObject.bind(internal);
export const isNumberObject = internal.isNumberObject.bind(internal);
export const isBooleanObject = internal.isBooleanObject.bind(internal);
export const isBigIntObject = internal.isBigIntObject.bind(internal);
export const isArrayBufferView = internal.isArrayBufferView.bind(internal);
export const isBigInt64Array = internal.isBigInt64Array.bind(internal);
export const isBigUint64Array = internal.isBigUint64Array.bind(internal);
export const isFloat32Array = internal.isFloat32Array.bind(internal);
export const isFloat64Array = internal.isFloat64Array.bind(internal);
export const isInt8Array = internal.isInt8Array.bind(internal);
export const isInt16Array = internal.isInt16Array.bind(internal);
export const isInt32Array = internal.isInt32Array.bind(internal);
export const isTypedArray = internal.isTypedArray.bind(internal);
export const isUint8Array = internal.isUint8Array.bind(internal);
export const isUint8ClampedArray = internal.isUint8ClampedArray.bind(internal);
export const isUint16Array = internal.isUint16Array.bind(internal);
export const isUint32Array = internal.isUint32Array.bind(internal);

export default {
  isCryptoKey,
  isKeyObject,

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
  isProxy,
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
  // TODO(soon): isExternal
};
