// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  PerformanceEntry,
  kSkipThrow,
} from 'node-internal:internal_performance_entry';
import {
  ERR_MISSING_ARGS,
  ERR_INVALID_ARG_VALUE,
  ERR_PERFORMANCE_INVALID_TIMESTAMP,
  ERR_METHOD_NOT_IMPLEMENTED,
  ERR_ILLEGAL_CONSTRUCTOR,
} from 'node-internal:internal_errors';
import {
  validateObject,
  validateNumber,
  validateThisInternalField,
} from 'node-internal:validators';
import { kEnumerableProperty } from 'node-internal:internal_utils';
import type {
  PerformanceMark as _PerformanceMark,
  EntryType,
} from 'node:perf_hooks';

const kDetail = Symbol('kDetail');

const nodeTimingReadOnlyAttributes = new Set([
  'nodeStart',
  'v8Start',
  'environment',
  'loopStart',
  'loopExit',
  'bootstrapComplete',
]);

// @ts-expect-error TS2720 Inconsistencies exist.
export class PerformanceMark
  extends PerformanceEntry
  implements _PerformanceMark
{
  constructor(
    name: string | number,
    options: PerformanceMarkOptions | undefined = undefined
  ) {
    if (arguments.length === 0) {
      throw new ERR_MISSING_ARGS('name');
    }
    name = `${name}`;
    if (nodeTimingReadOnlyAttributes.has(name)) {
      throw new ERR_INVALID_ARG_VALUE('name', name);
    }
    if (options != null) {
      validateObject(options, 'options');
    }
    const startTime = options?.startTime ?? Date.now();
    validateNumber(startTime, 'startTime');
    if (startTime < 0) throw new ERR_PERFORMANCE_INVALID_TIMESTAMP(startTime);
    super();
    throw new ERR_METHOD_NOT_IMPLEMENTED('PerformanceMark');
  }

  get detail(): unknown {
    validateThisInternalField(this, kDetail, 'PerformanceMark');
    return this[kDetail];
  }

  override toJSON(): Record<string, unknown> {
    return {
      name: this.name,
      entryType: this.entryType,
      startTime: this.startTime,
      duration: this.duration,
      detail: this[kDetail],
    };
  }
}

Object.defineProperties(PerformanceMark.prototype, {
  detail: kEnumerableProperty,
  [Symbol.toStringTag]: {
    // @ts-expect-error TS2353 using _proto_ is OK.
    __proto__: null,
    configurable: true,
    value: 'PerformanceMark',
  },
});

export class PerformanceMeasure extends PerformanceEntry {
  constructor(
    skipThrowSymbol: symbol | undefined = undefined,
    name: string | undefined = undefined,
    type: EntryType | undefined = undefined,
    start: number | undefined = undefined,
    duration: number | undefined = undefined
  ) {
    if (skipThrowSymbol !== kSkipThrow) {
      throw new ERR_ILLEGAL_CONSTRUCTOR();
    }

    super(skipThrowSymbol, name, type, start, duration);
  }

  get detail(): unknown {
    validateThisInternalField(this, kDetail, 'PerformanceMeasure');
    return this[kDetail];
  }

  override toJSON(): Record<string, unknown> {
    return {
      name: this.name,
      entryType: this.entryType,
      startTime: this.startTime,
      duration: this.duration,
      detail: this[kDetail],
    };
  }
}
Object.defineProperties(PerformanceMeasure.prototype, {
  detail: kEnumerableProperty,
  [Symbol.toStringTag]: {
    // @ts-expect-error TS2353 using _proto_ is OK.
    __proto__: null,
    configurable: true,
    value: 'PerformanceMeasure',
  },
});
