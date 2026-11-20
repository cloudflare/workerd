// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared by more than one behavior module.

import { ok, strictEqual } from 'node:assert';

export const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';

export const kMissingUrl =
  "Failed to execute 'sendBeacon' on 'Navigator': parameter 1 is not of " +
  "type 'string'.";

export const kSymbolArgument = 'Cannot convert a Symbol value to a string';

// A property descriptor's attributes as a short string: its kind ('data'
// or 'accessor'), then w, e and c for writable, enumerable and
// configurable.
export function attributesOf(object, key) {
  const descriptor = Object.getOwnPropertyDescriptor(object, key);
  if (descriptor === undefined) return 'none';
  return [
    'value' in descriptor ? 'data' : 'accessor',
    descriptor.writable ? 'w' : '',
    descriptor.enumerable ? 'e' : '',
    descriptor.configurable ? 'c' : '',
  ].join('');
}

// What `fn` throws, or undefined.
export function caught(fn) {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

// Asserts `fn` throws a TypeError of this realm with exactly `message`,
// and returns it.
export function throwsTypeError(fn, message) {
  const error = caught(fn);
  ok(error instanceof TypeError, `expected a TypeError, got ${error}`);
  strictEqual(Object.getPrototypeOf(error), TypeError.prototype);
  strictEqual(error.message, message);
  return error;
}

// The frames of an error's stack: its lines after the first.
export function framesOf(error) {
  return error.stack.split('\n').slice(1);
}

// Whether a stack frame is one of the per-isolate bootstrap's.
export function isBootstrapFrame(frame) {
  return frame.includes('workerd:per-isolate/');
}

// The sink service's record of the beacon sent to `path`, once it arrives.
export async function received(path) {
  for (let attempt = 0; attempt < 200; attempt++) {
    const response = await fetch(`http://sink/received?path=${path}`);
    if (response.status === 200) return await response.json();
    await scheduler.wait(5);
  }
  throw new Error(`no beacon arrived at ${path}`);
}
