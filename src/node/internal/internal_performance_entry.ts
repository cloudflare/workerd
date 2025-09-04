// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { ERR_ILLEGAL_CONSTRUCTOR } from 'node-internal:internal_errors';
import { validateThisInternalField } from 'node-internal:validators';
import { inspect, type InspectOptions } from 'node-internal:internal_inspect';
import { kEnumerableProperty } from 'node-internal:internal_utils';

import type {
  PerformanceEntry as _PerformanceEntry,
  EntryType,
} from 'node:perf_hooks';

const kInspect = inspect.custom;

const kName = Symbol('PerformanceEntry.Name');
const kEntryType = Symbol('PerformanceEntry.EntryType');
const kStartTime = Symbol('PerformanceEntry.StartTime');
const kDuration = Symbol('PerformanceEntry.Duration');
export const kSkipThrow = Symbol('kSkipThrow');

export class PerformanceEntry implements _PerformanceEntry {
  [kName]: string | undefined;
  [kEntryType]: string | undefined;
  [kStartTime]: number | undefined;
  [kDuration]: number | undefined;

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

    this[kName] = name;
    this[kEntryType] = type;
    this[kStartTime] = start;
    this[kDuration] = duration;
  }

  get name(): string {
    validateThisInternalField(this, kName, 'PerformanceEntry');
    return this[kName] as string;
  }

  get entryType(): EntryType {
    validateThisInternalField(this, kEntryType, 'PerformanceEntry');
    return this[kEntryType] as EntryType;
  }

  get startTime(): number {
    validateThisInternalField(this, kStartTime, 'PerformanceEntry');
    return this[kStartTime] as number;
  }

  get duration(): number {
    validateThisInternalField(this, kDuration, 'PerformanceEntry');
    return this[kDuration] as number;
  }

  [kInspect](depth: number, options: InspectOptions): this | string {
    if (depth < 0) return this;

    const opts = {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    };

    return `${this.constructor.name} ${inspect(this.toJSON(), opts)}`;
  }

  toJSON(): Record<string, unknown> {
    validateThisInternalField(this, kName, 'PerformanceEntry');
    return {
      name: this[kName],
      entryType: this[kEntryType],
      startTime: this[kStartTime],
      duration: this[kDuration],
    };
  }
}
Object.defineProperties(PerformanceEntry.prototype, {
  name: kEnumerableProperty,
  entryType: kEnumerableProperty,
  startTime: kEnumerableProperty,
  duration: kEnumerableProperty,
  toJSON: kEnumerableProperty,
});

export function createPerformanceEntry(
  name: string,
  type: EntryType,
  start: number,
  duration: number
): PerformanceEntry {
  return new PerformanceEntry(kSkipThrow, name, type, start, duration);
}
