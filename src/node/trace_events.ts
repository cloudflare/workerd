// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
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

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

import { validateObject, validateString } from 'node-internal:validators';

import { format, customInspectSymbol } from 'node-internal:internal_inspect';

import type { CreateTracingOptions } from 'node:trace_events';

// TODO(soon): It is conceivable that we might implement this as part of
// the workers observability features in the future. For now it's a non
// functional stub.

class Tracing {
  constructor(_: string[]) {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Tracing');
  }

  enable(): void {
    // non-op
  }

  disable(): void {
    // non-op
  }

  get enabled(): boolean {
    return false;
  }

  get categories(): string {
    return '';
  }

  [customInspectSymbol](depth?: number, _: object = {}): string | object {
    if (typeof depth === 'number' && depth < 0) return this;

    const obj = {
      enabled: this.enabled,
      categories: this.categories,
    };
    return `Tracing ${format(obj)}`;
  }
}

export function createTracing(options: CreateTracingOptions): Tracing {
  validateObject(options, 'options');
  validateString(options.categories, 'options.categories');
  throw new ERR_METHOD_NOT_IMPLEMENTED('trace_events.createTracing');
}

export function getEnabledCategories(): string {
  return '';
}

export default {
  createTracing,
  getEnabledCategories,
};
