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

import type tls from 'node:tls';
import { ERR_OPTION_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import { validateInteger } from 'node-internal:validators';

// @ts-expect-error TS2323 Redeclare error.
export declare class SecureContext {
  public context: unknown;
  public constructor(
    _secureProtocol?: string,
    secureOptions?: number,
    minVersion?: string,
    maxVersion?: string
  );
}

// This is intentionally not fully compatible with Node.js implementation
// since creating and customizing an actually equivalent SecureContext is not supported.
// @ts-expect-error TS2323 Redeclare error.
export function SecureContext(
  this: SecureContext,
  secureProtocol?: string,
  secureOptions?: number,
  minVersion?: string,
  maxVersion?: string
): SecureContext {
  if (!(this instanceof SecureContext)) {
    return new SecureContext(
      secureProtocol,
      secureOptions,
      minVersion,
      maxVersion
    );
  }
  if (minVersion !== undefined) {
    throw new ERR_OPTION_NOT_IMPLEMENTED('minVersion');
  }
  if (maxVersion !== undefined) {
    throw new ERR_OPTION_NOT_IMPLEMENTED('maxVersion');
  }
  if (secureOptions) {
    validateInteger(secureOptions, 'secureOptions');
  }

  this.context = undefined;
  return this;
}

export function createSecureContext(
  options: tls.SecureContextOptions = {}
): SecureContext {
  const nonNullEntry = Object.entries(options).find(
    ([_key, value]) => value != null
  );
  if (nonNullEntry) {
    throw new ERR_OPTION_NOT_IMPLEMENTED(`options.${nonNullEntry[0]}`);
  }
  return new SecureContext(
    options.secureProtocol,
    options.secureOptions,
    options.minVersion,
    options.maxVersion
  );
}
