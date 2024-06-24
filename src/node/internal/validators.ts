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
/* eslint-disable */
//import { codes } from "node-internal:error-codes";

// import { hideStackFrames } from "node-internal:hide-stack-frames";
import { isArrayBufferView } from "node-internal:internal_types";
import { normalizeEncoding } from "node-internal:internal_utils";
import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_SOCKET_BAD_PORT,
  ERR_OUT_OF_RANGE,
} from "node-internal:internal_errors";

// TODO(someday): Not current implementing parseFileMode

export const isInt32 = (value: any) => value === (value | 0);
export const isUint32 = (value: any) => value === (value >>> 0);

export function validateBuffer(buffer: unknown, name = "buffer") {
  if (!isArrayBufferView(buffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      name,
      ["Buffer", "TypedArray", "DataView"],
      buffer,
    );
  }
};

export function validateInteger(
    value: unknown,
    name: string,
    min = Number.MIN_SAFE_INTEGER,
    max = Number.MAX_SAFE_INTEGER) {
  if (typeof value !== "number") {
    throw new ERR_INVALID_ARG_TYPE(name, "number", value);
  }
  if (!Number.isInteger(value)) {
    throw new ERR_OUT_OF_RANGE(name, "an integer", value);
  }
  if (value < min || value > max) {
    throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
  }
}

export interface ValidateObjectOptions {
  allowArray?: boolean;
  allowFunction?: boolean;
  nullable?: boolean;
};

export function validateObject(value: unknown, name: string, options?: ValidateObjectOptions) {
  const useDefaultOptions = options == null;
  const allowArray = useDefaultOptions ? false : options.allowArray;
  const allowFunction = useDefaultOptions ? false : options.allowFunction;
  const nullable = useDefaultOptions ? false : options.nullable;
  if (
    (!nullable && value === null) ||
    (!allowArray && Array.isArray(value)) ||
    (typeof value !== "object" && (
      !allowFunction || typeof value !== "function"
    ))
  ) {
    throw new ERR_INVALID_ARG_TYPE(name, "Object", value);
  }
};

export function validateInt32(value: any, name: string, min = -2147483648, max = 2147483647) {
  if (!isInt32(value)) {
    if (typeof value !== "number") {
      throw new ERR_INVALID_ARG_TYPE(name, "number", value);
    }

    if (!Number.isInteger(value)) {
      throw new ERR_OUT_OF_RANGE(name, "an integer", value);
    }

    throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
  }

  if (value < min || value > max) {
    throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
  }
}

export function validateUint32(value: unknown, name: string, positive?: boolean) {
  if (!isUint32(value)) {
    if (typeof value !== "number") {
      throw new ERR_INVALID_ARG_TYPE(name, "number", value);
    }
    if (!Number.isInteger(value)) {
      throw new ERR_OUT_OF_RANGE(name, "an integer", value);
    }
    const min = positive ? 1 : 0;
    // 2 ** 32 === 4294967296
    throw new ERR_OUT_OF_RANGE(
      name,
      `>= ${min} && < 4294967296`,
      value,
    );
  }
  if (positive && value === 0) {
    throw new ERR_OUT_OF_RANGE(name, ">= 1 && < 4294967296", value);
  }
}

export function validateString(value: unknown, name: string) {
  if (typeof value !== "string") {
    throw new ERR_INVALID_ARG_TYPE(name, "string", value);
  }
}

export function validateNumber(value: unknown, name: string) {
  if (typeof value !== "number") {
    throw new ERR_INVALID_ARG_TYPE(name, "number", value);
  }
}

export function validateBoolean(value: unknown, name: string) {
  if (typeof value !== "boolean") {
    throw new ERR_INVALID_ARG_TYPE(name, "boolean", value);
  }
}

export function validateOneOf(value: unknown, name: string, oneOf: any[]) {
  if (!Array.prototype.includes.call(oneOf, value)) {
    const allowed = Array.prototype.join.call(
      Array.prototype.map.call(
        oneOf,
        (v) => (typeof v === "string" ? `'${v}'` : String(v)),
      ),
      ", ",
    );
    const reason = "must be one of: " + allowed;

    throw new ERR_INVALID_ARG_VALUE(name, value, reason);
  }
}

export function validateEncoding(data: unknown, encoding: string): void {
  const normalizedEncoding = normalizeEncoding(encoding);
  const length = (data as any).length;

  if (normalizedEncoding === "hex" && length % 2 !== 0) {
    throw new ERR_INVALID_ARG_VALUE(
      "encoding",
      encoding,
      `is invalid for data of length ${length}`,
    );
  }
}

export function validateAbortSignal(signal: unknown, name: string) {
  if (
    signal !== undefined &&
    (signal === null ||
      typeof signal !== "object" ||
      !("aborted" in signal))
  ) {
    throw new ERR_INVALID_ARG_TYPE(name, "AbortSignal", signal);
  }
};

export function validateFunction(value: unknown, name: string) {
  if (typeof value !== "function") {
    throw new ERR_INVALID_ARG_TYPE(name, "Function", value);
  }
}

export function validateArray(value: unknown, name: string, minLength = 0) {
  if (!Array.isArray(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, "Array", value);
  }
  if (value.length < minLength) {
    const reason = `must be longer than ${minLength}`;
    throw new ERR_INVALID_ARG_VALUE(name, value, reason);
  }
}

export function validatePort(port: unknown, name = 'Port', allowZero = true) {
  if ((typeof port !== 'number' && typeof port !== 'string') ||
      (typeof port === 'string' && port.trim().length === 0) ||
      +port !== (+port >>> 0) ||
      +port > 0xFFFF ||
      (port === 0 && !allowZero)) {
    throw new ERR_SOCKET_BAD_PORT(name, port, allowZero);
  }
  return +port | 0;
};

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
};
