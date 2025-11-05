// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

/* eslint-disable @typescript-eslint/no-unsafe-assignment */

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import { default as util } from 'node-internal:util';

export const constants = Object.freeze({
  NODE_PERFORMANCE_GC_MAJOR: 4,
  NODE_PERFORMANCE_GC_MINOR: 1,
  NODE_PERFORMANCE_GC_INCREMENTAL: 8,
  NODE_PERFORMANCE_GC_WEAKCB: 16,
  NODE_PERFORMANCE_GC_FLAGS_NO: 0,
  NODE_PERFORMANCE_GC_FLAGS_CONSTRUCT_RETAINED: 2,
  NODE_PERFORMANCE_GC_FLAGS_FORCED: 4,
  NODE_PERFORMANCE_GC_FLAGS_SYNCHRONOUS_PHANTOM_PROCESSING: 8,
  NODE_PERFORMANCE_GC_FLAGS_ALL_AVAILABLE_GARBAGE: 16,
  NODE_PERFORMANCE_GC_FLAGS_ALL_EXTERNAL_MEMORY: 32,
  NODE_PERFORMANCE_GC_FLAGS_SCHEDULE_IDLE: 64,
});

export const performance = globalThis.performance;

export function createHistogram(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('createHistogram');
}

export function monitorEventLoopDelay(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('monitorEventLoopDelay');
}

export const Performance = util.Performance;
export const PerformanceEntry = util.PerformanceEntry;
export const PerformanceMeasure = util.PerformanceMeasure;
export const PerformanceMark = util.PerformanceMark;
export const PerformanceObserver = util.PerformanceObserver;
export const PerformanceObserverEntryList = util.PerformanceObserverEntryList;
export const PerformanceResourceTiming = util.PerformanceResourceTiming;

// We intentionally not actually implement support for perf_hooks feedback into the worker.
// Even though, classes like PerformanceEntry and PerformanceObserver are here, they are
// not actually expected to be used for anything, at least not for the foreseeable future.
export default {
  Performance,
  PerformanceEntry,
  PerformanceMark,
  PerformanceMeasure,
  PerformanceObserver,
  PerformanceObserverEntryList,
  PerformanceResourceTiming,
  monitorEventLoopDelay,
  createHistogram,
  performance,
  constants,
};
