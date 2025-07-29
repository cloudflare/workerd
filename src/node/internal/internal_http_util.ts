// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.
import type { IncomingMessage } from 'node:http';

export type IncomingMessageCallback = (req: IncomingMessage) => void;

export function once<RT>(
  this: unknown,
  callback: (...allArgs: unknown[]) => RT,
  { preserveReturnValue = false } = {}
): (...all: unknown[]) => RT {
  let called = false;
  let returnValue: RT;
  return function (this: unknown, ...args: unknown[]): RT {
    if (called) return returnValue;
    called = true;
    const result = Reflect.apply(callback, this, args);
    returnValue = preserveReturnValue ? result : (undefined as RT);
    return result;
  };
}

export const kServerResponse = Symbol('ServerResponse');
export const kIncomingMessage = Symbol('IncomingMessage');

// RFC 7230 compliant header value splitting that respects quoted-string constructions
// Spec: https://www.rfc-editor.org/rfc/rfc7230.html#appendix-B
// Edge cases handled:
// - Quoted strings with commas: 'text/plain; param="a, b"' -> 'text/plain; param="a, b"'
// - Escaped quotes in strings: 'text/plain; param="a \"quoted\" value"' -> 'text/plain; param="a \"quoted\" value"'
// - Multiple values: 'text/plain; f="a, b", text/html' -> 'text/plain; f="a, b"'
// - Whitespace before commas: 'value1 \t , value2' -> 'value1'
// - No commas: 'single-value' -> 'single-value'
export function splitHeaderValue(value: string): string {
  let inQuotes = false;

  for (let i = 0; i < value.length; i++) {
    const char = value[i];

    if (char === '"') {
      inQuotes = !inQuotes;
    } else if (char === '\\' && inQuotes) {
      i++; // Skip next character (it's escaped)
    } else if (!inQuotes && char === ',') {
      // Found unquoted comma, trim whitespace and return
      return value.slice(0, i).trimEnd();
    }
  }

  return value;
}
