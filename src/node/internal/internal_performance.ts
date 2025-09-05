// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  ERR_METHOD_NOT_IMPLEMENTED,
  ERR_ILLEGAL_CONSTRUCTOR,
  ERR_MISSING_ARGS,
} from 'node-internal:internal_errors';
import { PerformanceEntry } from 'node-internal:internal_performance_entry';
import { validateThisInternalField } from 'node-internal:validators';
import { kEnumerableProperty } from 'node-internal:internal_utils';
import { inspect } from 'node-internal:internal_inspect';
import type { InspectOptions } from 'node-internal:internal_inspect';
import type {
  Performance as _Performance,
  PerformanceMark as _PerformanceMark,
  PerformanceNodeTiming,
  MarkOptions,
  EventLoopUtilization,
} from 'node:perf_hooks';

const kInspect = inspect.custom;
const kPerformanceBrand = Symbol('performance');
const kInitialize = Symbol('initialize');

export class Performance extends EventTarget implements _Performance {
  // @ts-expect-error TS2564 We don't implement performance node timing.
  nodeTiming: PerformanceNodeTiming;
  [kPerformanceBrand] = true;

  constructor(arg: symbol | undefined = undefined) {
    super();
    if (arg !== kInitialize) {
      throw new ERR_ILLEGAL_CONSTRUCTOR();
    }
  }

  [kInspect](depth: number, options: InspectOptions): this | string {
    if (depth < 0) return this;

    const opts = {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    };

    return `Performance ${inspect(
      {
        nodeTiming: this.nodeTiming,
        timeOrigin: this.timeOrigin,
      },
      opts
    )}`;
  }

  clearMarks(_name?: string): void {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    // Intentionally left as a no-op.
  }

  clearMeasures(_name?: string): void {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    // Intentionally left as a no-op.
  }

  clearResourceTimings(_name?: string): void {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    // Intentionally left as a no-op.
  }

  getEntries(): PerformanceEntry[] {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    return [];
  }

  getEntriesByName(_name: string, _type?: string): PerformanceEntry[] {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    if (arguments.length === 0) {
      throw new ERR_MISSING_ARGS('name');
    }
    return [];
  }

  getEntriesByType(_type: string): PerformanceEntry[] {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    if (arguments.length === 0) {
      throw new ERR_MISSING_ARGS('type');
    }
    return [];
  }

  // @ts-expect-error TS2416 return types are different.
  mark(_name: string, _options: MarkOptions | undefined = {}): PerformanceMark {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    if (arguments.length === 0) {
      throw new ERR_MISSING_ARGS('name');
    }
    throw new ERR_METHOD_NOT_IMPLEMENTED('Performance.mark');
  }

  // @ts-expect-error TS2416 missing overloads. correct this soon.
  measure(
    _name: string,
    _startOrMeasureOptions: string | undefined | Record<string, unknown> = {},
    _endMark: string | undefined = undefined
  ): void {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    if (arguments.length === 0) {
      throw new ERR_MISSING_ARGS('name');
    }
    throw new ERR_METHOD_NOT_IMPLEMENTED('Performance.measure');
  }

  now(): number {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    return globalThis.performance.now();
  }

  setResourceTimingBufferSize(_maxSize: number): void {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    if (arguments.length === 0) {
      throw new ERR_MISSING_ARGS('maxSize');
    }
    throw new ERR_METHOD_NOT_IMPLEMENTED(
      'Performance.setResourceTimingBufferSize'
    );
  }

  get timeOrigin(): number {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    return globalThis.performance.timeOrigin;
  }

  toJSON(): Record<string, unknown> {
    validateThisInternalField(this, kPerformanceBrand, 'Performance');
    return {
      nodeTiming: this.nodeTiming,
      timeOrigin: this.timeOrigin,
      eventLoopUtilization: this.eventLoopUtilization(),
    };
  }

  eventLoopUtilization(): EventLoopUtilization {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Performance.eventLoopUtilization');
  }
}

Object.defineProperties(Performance.prototype, {
  clearMarks: kEnumerableProperty,
  clearMeasures: kEnumerableProperty,
  clearResourceTimings: kEnumerableProperty,
  getEntries: kEnumerableProperty,
  getEntriesByName: kEnumerableProperty,
  getEntriesByType: kEnumerableProperty,
  mark: kEnumerableProperty,
  measure: kEnumerableProperty,
  now: kEnumerableProperty,
  timeOrigin: kEnumerableProperty,
  toJSON: kEnumerableProperty,
  setResourceTimingBufferSize: kEnumerableProperty,
  [Symbol.toStringTag]: {
    // @ts-expect-error TS2353 using _proto_ is OK.
    __proto__: null,
    writable: false,
    enumerable: false,
    configurable: true,
    value: 'Performance',
  },

  // Node.js specific extensions.
  eventLoopUtilization: kEnumerableProperty,
  nodeTiming: {
    // @ts-expect-error TS2353 using _proto_ is OK.
    __proto__: null,
    configurable: true,
    // Node.js specific extensions.
    enumerable: false,
    writable: true,
    value: {},
  },
  // In the browser, this function is not public.  However, it must be used inside fetch
  // which is a Node.js dependency, not a internal module
  markResourceTiming: {
    // @ts-expect-error TS2353 using _proto_ is OK.
    __proto__: null,
    configurable: true,
    // Node.js specific extensions.
    enumerable: false,
    writable: true,
    value: () => {
      throw new ERR_METHOD_NOT_IMPLEMENTED('Performance.markResourceTiming');
    },
  },
  timerify: {
    // @ts-expect-error TS2353 using _proto_ is OK.
    __proto__: null,
    configurable: true,
    // Node.js specific extensions.
    enumerable: false,
    writable: true,
    value: () => {
      throw new ERR_METHOD_NOT_IMPLEMENTED('Performance.timerify');
    },
  },
});

export function createPerformance(): Performance {
  return new Performance(kInitialize);
}
