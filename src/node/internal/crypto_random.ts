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

import { default as cryptoImpl } from 'node-internal:crypto';

import {
  validateObject,
  validateBoolean,
  validateFunction,
  validateInt32,
  validateInteger,
} from 'node-internal:validators';

import {
  isAnyArrayBuffer,
  isArrayBufferView,
} from 'node-internal:internal_types';

import {
  ERR_INVALID_ARG_TYPE,
  ERR_OUT_OF_RANGE,
} from 'node-internal:internal_errors';

import { Buffer, kMaxLength } from 'node-internal:internal_buffer';

import { arrayBufferToUnsignedBigInt } from 'node-internal:crypto_util';
import type { RandomUUIDOptions } from 'node:crypto';

export type RandomBytesCallback = (
  err: Error | null,
  buffer: Uint8Array
) => void;
export function randomBytes(size: number, callback: RandomBytesCallback): void;
export function randomBytes(size: number): Uint8Array;
export function randomBytes(
  size: number,
  callback?: RandomBytesCallback
): Uint8Array | undefined {
  validateInteger(size, 'size', 0, kMaxLength);
  const buf = Buffer.alloc(size);
  if (callback !== undefined) {
    randomFill(buf, callback as RandomFillCallback);
    return undefined;
  } else {
    randomFillSync(buf);
    return buf;
  }
}

export function randomFillSync(
  buffer: NodeJS.ArrayBufferView,
  offset?: number,
  size?: number
): Uint8Array {
  if (!isAnyArrayBuffer(buffer) && !isArrayBufferView(buffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      'buffer',
      ['TypedArray', 'DataView', 'ArrayBuffer', 'SharedArrayBuffer'],
      buffer
    );
  }
  const maxLength = (buffer as Uint8Array).length;
  if (offset !== undefined) {
    validateInteger(offset, 'offset', 0, kMaxLength);
  } else offset = 0;
  if (size !== undefined) {
    validateInteger(size, 'size', 0, maxLength - offset);
  } else size = maxLength - offset;
  if (isAnyArrayBuffer(buffer)) {
    buffer = Buffer.from(buffer);
  }
  buffer = (buffer as Buffer).subarray(offset, offset + size);
  return crypto.getRandomValues(buffer);
}

export type RandomFillCallback = (
  err: Error | null,
  buf?: NodeJS.ArrayBufferView
) => void;
export function randomFill(
  buffer: NodeJS.ArrayBufferView,
  callback?: RandomFillCallback
): void;
export function randomFill(
  buffer: NodeJS.ArrayBufferView,
  offset: number,
  callback?: RandomFillCallback
): void;
export function randomFill(
  buffer: NodeJS.ArrayBufferView,
  offset: number,
  size: number,
  callback?: RandomFillCallback
): void;
export function randomFill(
  buffer: NodeJS.ArrayBufferView,
  offsetOrCallback?: number | RandomFillCallback,
  sizeOrCallback?: number | RandomFillCallback,
  callback?: RandomFillCallback
): void {
  if (!isAnyArrayBuffer(buffer) && !isArrayBufferView(buffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      'buffer',
      ['TypedArray', 'DataView', 'ArrayBuffer', 'SharedArrayBuffer'],
      buffer
    );
  }

  let offset = 0;
  let size = 0;
  const maxLength = (buffer as Uint8Array).length;
  if (typeof callback === 'function') {
    validateInteger(offsetOrCallback, 'offset', 0, maxLength);
    offset = offsetOrCallback;

    validateInteger(sizeOrCallback, 'size', 0, maxLength - offset);
    size = sizeOrCallback;
  } else if (typeof sizeOrCallback === 'function') {
    validateInteger(offsetOrCallback, 'offset', 0, maxLength);
    offset = offsetOrCallback;
    size = maxLength - offset;
    callback = sizeOrCallback;
  } else if (typeof offsetOrCallback === 'function') {
    offset = 0;
    size = maxLength;
    callback = offsetOrCallback;
  }
  validateFunction(callback, 'callback');

  // We're currently not actually implementing the fill itself asynchronously,
  // so we defer to randomFillSync here, but we invoke the callback asynchronously.
  new Promise<void>((res) => {
    randomFillSync(buffer, offset, size);
    res();
  }).then(
    (): unknown => callback(null, buffer),
    (err: unknown): unknown => callback(err as Error)
  );
}

const RAND_MAX = 0xffff_ffff_ffff;
// Cache random data to use in randomInt. The cache size must be evenly
// divisible by 6 because each attempt to obtain a random int uses 6 bytes.
const randomCache = Buffer.alloc(6 * 1024);
let randomCacheOffset = 0;
let initialized = false;

function getRandomInt(min: number, max: number): number {
  if (!initialized) {
    randomFillSync(randomCache);
    initialized = true;
  }
  // First we generate a random int between [0..range)
  const range = max - min;

  if (!(range <= RAND_MAX)) {
    throw new ERR_OUT_OF_RANGE(
      `max${max ? '' : ' - min'}`,
      `<= ${RAND_MAX}`,
      range
    );
  }

  // For (x % range) to produce an unbiased value greater than or equal to 0 and
  // less than range, x must be drawn randomly from the set of integers greater
  // than or equal to 0 and less than randLimit.
  const randLimit = RAND_MAX - (RAND_MAX % range);

  // If we don't have a callback, or if there is still data in the cache, we can
  // do this synchronously, which is super fast.
  while (randomCacheOffset <= randomCache.length) {
    if (randomCacheOffset === randomCache.length) {
      // This might block the thread for a bit, but we are in sync mode.
      randomFillSync(randomCache);
      randomCacheOffset = 0;
    }

    const x = randomCache.readUIntBE(randomCacheOffset, 6);
    randomCacheOffset += 6;
    if (x < randLimit) {
      return (x % range) + min;
    }
  }
  return 0; // Should be unreachable.
}

export type RandomIntCallback = (err: Error | null, n?: number) => void;
export function randomInt(max: number): number;
export function randomInt(min: number, max: number): number;
export function randomInt(max: number, callback: RandomIntCallback): void;
export function randomInt(
  min: number,
  max: number,
  callback: RandomIntCallback
): void;
export function randomInt(
  minOrMax: number,
  maxOrCallback?: number | RandomIntCallback,
  callback?: RandomIntCallback
): number | undefined {
  let min = 0;
  let max = 0;
  if (typeof callback === 'function') {
    validateInteger(minOrMax, 'min');
    validateInteger(maxOrCallback, 'max');
    min = minOrMax;
    max = maxOrCallback;
  } else if (typeof maxOrCallback === 'function') {
    min = 0;
    validateInteger(minOrMax, 'max');
    max = minOrMax;
    callback = maxOrCallback;
  } else if (arguments.length === 2) {
    validateInteger(minOrMax, 'min');
    validateInteger(maxOrCallback, 'max');
    min = minOrMax;
    max = maxOrCallback;
  } else {
    min = 0;
    validateInteger(minOrMax, 'max');
    max = minOrMax;
  }

  if (min >= max) {
    throw new ERR_OUT_OF_RANGE('min', 'min < max', min);
  }

  if (callback != null) {
    new Promise<number>((res) => {
      res(getRandomInt(min, max));
    }).then(
      (n: number): void => {
        callback(null, n);
      },
      (err: unknown): void => {
        callback(err as Error);
      }
    );
  } else {
    return getRandomInt(min, max);
  }

  return undefined;
}

export function randomUUID(options?: RandomUUIDOptions): string {
  // While we do not actually use the entropy cache, we go ahead and validate
  // the input parameters as Node.js does.
  if (options !== undefined) {
    validateObject(options, 'options');
    if (options.disableEntropyCache !== undefined) {
      validateBoolean(
        options.disableEntropyCache,
        'options.disableEntropyCache'
      );
    }
  }
  return crypto.randomUUID();
}

export type PrimeNum = ArrayBuffer | ArrayBufferView | Buffer | bigint;
export interface GeneratePrimeOptions {
  add?: PrimeNum;
  rem?: PrimeNum;
  safe?: boolean;
  bigint?: boolean;
}

export interface CheckPrimeOptions {
  checks?: number;
}

export type GeneratePrimeCallback = (
  err: Error | null,
  prime?: bigint | ArrayBuffer
) => void;
export type CheckPrimeCallback = (err: Error | null, prime?: boolean) => void;

function processGeneratePrimeOptions(options: GeneratePrimeOptions): {
  add: ArrayBufferView;
  rem: ArrayBufferView;
  safe: boolean;
  bigint: boolean;
} {
  validateObject(options, 'options');
  const { safe = false, bigint = false } = options;
  let { add, rem } = options;
  validateBoolean(safe, 'options.safe');
  validateBoolean(bigint, 'options.bigint');

  if (add !== undefined) {
    if (typeof add === 'bigint') {
      add = unsignedBigIntToBuffer(add, 'options.add');
    } else if (!isAnyArrayBuffer(add) && !isArrayBufferView(add)) {
      throw new ERR_INVALID_ARG_TYPE(
        'options.add',
        ['ArrayBuffer', 'TypedArray', 'Buffer', 'DataView', 'bigint'],
        add
      );
    }
  }

  if (rem !== undefined) {
    if (typeof rem === 'bigint') {
      rem = unsignedBigIntToBuffer(rem, 'options.rem');
    } else if (!isAnyArrayBuffer(rem) && !isArrayBufferView(rem)) {
      throw new ERR_INVALID_ARG_TYPE(
        'options.rem',
        ['ArrayBuffer', 'TypedArray', 'Buffer', 'DataView', 'bigint'],
        rem
      );
    }
  }

  return {
    safe,
    bigint,
    add: add as ArrayBufferView,
    rem: rem as ArrayBufferView,
  };
}

export function generatePrimeSync(
  size: number,
  options: GeneratePrimeOptions = {}
): bigint | ArrayBuffer {
  validateInt32(size, 'size', 1);
  const { safe, bigint, add, rem } = processGeneratePrimeOptions(options);

  const primeBuf = cryptoImpl.randomPrime(size, safe, add, rem);
  return bigint ? arrayBufferToUnsignedBigInt(primeBuf) : primeBuf;
}

export function generatePrime(
  size: number,
  options: GeneratePrimeOptions,
  callback: GeneratePrimeCallback
): void;
export function generatePrime(
  size: number,
  callback: GeneratePrimeCallback
): void;
export function generatePrime(
  size: number,
  options: GeneratePrimeOptions | GeneratePrimeCallback,
  callback?: GeneratePrimeCallback
): void {
  validateInt32(size, 'size', 1);
  if (typeof options === 'function') {
    callback = options;
    options = {};
  }
  validateFunction(callback, 'callback');

  const { safe, bigint, add, rem } = processGeneratePrimeOptions(options);

  new Promise<bigint | ArrayBuffer>((res, rej) => {
    try {
      const primeBuf = cryptoImpl.randomPrime(size, safe, add, rem);
      res(bigint ? arrayBufferToUnsignedBigInt(primeBuf) : primeBuf);
    } catch (err) {
      rej(err as Error);
    }
  }).then(
    (val: bigint | ArrayBuffer): void => {
      callback(null, val);
    },
    (err: unknown): void => {
      callback(err as Error);
    }
  );
}

function unsignedBigIntToBuffer(bigint: bigint, name: string): Buffer {
  if (bigint < 0) {
    throw new ERR_OUT_OF_RANGE(name, '>= 0', bigint);
  }

  const hex = bigint.toString(16);
  const padded = hex.padStart(hex.length + (hex.length % 2), '0');
  return Buffer.from(padded, 'hex');
}

function validateCandidate(candidate: PrimeNum): Buffer {
  if (typeof candidate === 'bigint')
    candidate = unsignedBigIntToBuffer(candidate, 'candidate');
  if (!isAnyArrayBuffer(candidate) && !isArrayBufferView(candidate)) {
    throw new ERR_INVALID_ARG_TYPE(
      'candidate',
      ['ArrayBuffer', 'TypedArray', 'Buffer', 'DataView', 'bigint'],
      candidate
    );
  }
  return candidate as Buffer;
}

function validateChecks(options: CheckPrimeOptions): number {
  const { checks = 0 } = options;
  // The checks option is unsigned but must fit into a signed 32-bit integer for OpenSSL.
  validateInt32(checks, 'options.checks', 0);
  return checks;
}

export function checkPrimeSync(
  candidate: PrimeNum,
  options: CheckPrimeOptions = {}
): boolean {
  candidate = validateCandidate(candidate);
  validateObject(options, 'options');
  const checks = validateChecks(options);
  return cryptoImpl.checkPrimeSync(candidate as ArrayBufferView, checks);
}

export function checkPrime(
  candidate: PrimeNum,
  options: CheckPrimeOptions,
  callback: CheckPrimeCallback
): void;
export function checkPrime(
  candidate: PrimeNum,
  callback: CheckPrimeCallback
): void;
export function checkPrime(
  candidate: PrimeNum,
  options: CheckPrimeOptions | CheckPrimeCallback,
  callback?: CheckPrimeCallback
): void {
  candidate = validateCandidate(candidate);
  if (typeof options === 'function') {
    callback = options;
    options = {};
  }
  validateObject(options, 'options');
  validateFunction(callback, 'callback');
  const checks = validateChecks(options);
  new Promise<boolean>((res, rej) => {
    try {
      res(cryptoImpl.checkPrimeSync(candidate as ArrayBufferView, checks));
    } catch (err) {
      rej(err as Error);
    }
  }).then(
    (val: boolean): void => {
      callback(null, val);
    },
    (err: unknown): void => {
      callback(err);
    }
  );
}
