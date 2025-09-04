// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  ERR_ILLEGAL_CONSTRUCTOR,
  ERR_METHOD_NOT_IMPLEMENTED,
  ERR_MISSING_ARGS,
  ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';
import { kSkipThrow } from 'node-internal:internal_performance_entry';
import {
  validateThisInternalField,
  validateFunction,
  validateObject,
} from 'node-internal:validators';
import { inspect } from 'node-internal:internal_inspect';
import { kEnumerableProperty } from 'node:internal/internal_utils';
import type {
  PerformanceObserverEntryList as _PerformanceObserverEntryList,
  PerformanceObserver as _PerformanceObserver,
  PerformanceEntry,
} from 'node:perf_hooks';

const kInspect = inspect.custom;

const kBuffer = Symbol('kBuffer');
const kDispatch = Symbol('kDispatch');
const kMaybeBuffer = Symbol('kMaybeBuffer');

const kObservers = new Set();
const kPending = new Set();

const kSupportedEntryTypes = Object.freeze([
  'dns',
  'function',
  'gc',
  'http',
  'http2',
  'mark',
  'measure',
  'net',
  'resource',
]);

const performanceObserverSorter = (
  first: PerformanceEntry,
  second: PerformanceEntry
): number => {
  return first.startTime - second.startTime;
};

export class PerformanceObserverEntryList
  implements _PerformanceObserverEntryList
{
  [kBuffer]: PerformanceEntry[];

  constructor(
    skipThrowSymbol: symbol | undefined = undefined,
    entries: PerformanceEntry[] = []
  ) {
    if (skipThrowSymbol !== kSkipThrow) {
      throw new ERR_ILLEGAL_CONSTRUCTOR();
    }

    this[kBuffer] = entries.sort(performanceObserverSorter);
  }

  getEntries(): PerformanceEntry[] {
    validateThisInternalField(this, kBuffer, 'PerformanceObserverEntryList');
    return this[kBuffer].slice();
  }

  getEntriesByType(_type: string): PerformanceEntry[] {
    validateThisInternalField(this, kBuffer, 'PerformanceObserverEntryList');
    if (arguments.length === 0) {
      throw new ERR_MISSING_ARGS('type');
    }
    return [];
  }

  getEntriesByName(_name: string, _type = undefined): PerformanceEntry[] {
    validateThisInternalField(this, kBuffer, 'PerformanceObserverEntryList');
    if (arguments.length === 0) {
      throw new ERR_MISSING_ARGS('name');
    }
    return [];
  }

  [kInspect](depth: number, options: { depth?: number }): string | this {
    if (depth < 0) return this;

    const opts = {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    };

    return `PerformanceObserverEntryList ${inspect(this[kBuffer], opts)}`;
  }
}
Object.defineProperties(PerformanceObserverEntryList.prototype, {
  getEntries: kEnumerableProperty,
  getEntriesByType: kEnumerableProperty,
  getEntriesByName: kEnumerableProperty,
  [Symbol.toStringTag]: {
    // @ts-expect-error TS2353 using _proto_ is OK.
    __proto__: null,
    writable: false,
    enumerable: false,
    configurable: true,
    value: 'PerformanceObserverEntryList',
  },
});

// @ts-expect-error TS2720 Inconsistencies exist.
export class PerformanceObserver implements _PerformanceObserver {
  #buffer = [];
  #entryTypes = new Set<string>();
  // @ts-expect-error TS6133 Not used.
  // eslint-disable-next-line no-unused-private-class-members
  #callback: VoidFunction;

  constructor(callback: VoidFunction) {
    validateFunction(callback, 'callback');
    this.#callback = callback;
  }

  observe(options = {}): void {
    validateObject(options, 'options');
    const { entryTypes, type } = { ...options };
    if (entryTypes === undefined && type === undefined)
      throw new ERR_MISSING_ARGS('options.entryTypes', 'options.type');
    if (entryTypes != null && type != null)
      throw new ERR_INVALID_ARG_VALUE(
        'options.entryTypes',
        entryTypes,
        'options.entryTypes can not set with ' + 'options.type together'
      );

    throw new ERR_METHOD_NOT_IMPLEMENTED('PerformanceObserver.observe');
  }

  disconnect(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('PerformanceObserver.disconnect');
  }

  takeRecords(): PerformanceEntry[] {
    return [];
  }

  static get supportedEntryTypes(): readonly string[] {
    return kSupportedEntryTypes;
  }

  [kMaybeBuffer](_entry: PerformanceEntry): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('PerformanceObserver[kMaybeBuffer]');
  }

  [kDispatch](): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('PerformanceObserver[kDispatch]');
  }

  [kInspect](depth: number, options: { depth?: number }): this | string {
    if (depth < 0) return this;

    const opts = {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    };

    return `PerformanceObserver ${inspect(
      {
        connected: kObservers.has(this),
        pending: kPending.has(this),
        entryTypes: Array.from(this.#entryTypes),
        buffer: this.#buffer,
      },
      opts
    )}`;
  }
}
Object.defineProperties(PerformanceObserver.prototype, {
  observe: kEnumerableProperty,
  disconnect: kEnumerableProperty,
  takeRecords: kEnumerableProperty,
  [Symbol.toStringTag]: {
    // @ts-expect-error TS2353 using _proto_ is OK.
    __proto__: null,
    writable: false,
    enumerable: false,
    configurable: true,
    value: 'PerformanceObserver',
  },
});
