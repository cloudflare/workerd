// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

// Every file runs in the same isolate, so marks and measures created by one file would otherwise
// be visible to the next.
function clearEntries(): void {
  performance.clearMarks();
  performance.clearMeasures();
}

export default {
  'buffered-flag.any.js': {
    before: clearEntries,
    comment:
      'PerformanceObserver.observe() is a no-op in workerd, so the observer callbacks never run and the tests hang',
    disabledTests: [
      'PerformanceObserver with buffered flag sees previous marks',
      'PerformanceObserver with buffered flag sees previous measures',
    ],
  },
  'case-sensitivity.any.js': { before: clearEntries },
  'clear_all_marks.any.js': { before: clearEntries },
  'clear_all_measures.any.js': { before: clearEntries },
  'clear_non_existent_mark.any.js': { before: clearEntries },
  'clear_non_existent_measure.any.js': { before: clearEntries },
  'clear_one_mark.any.js': { before: clearEntries },
  'clear_one_measure.any.js': { before: clearEntries },
  'entry_type.any.js': { before: clearEntries },
  'idlharness.any.js': {
    before: clearEntries,
    comment:
      'The PerformanceMark and PerformanceMeasure interface objects do not have PerformanceEntry as their [[Prototype]]',
    expectedFailures: [
      'PerformanceMark interface: existence and properties of interface object',
      'PerformanceMeasure interface: existence and properties of interface object',
    ],
  },
  'invoke_with_timing_attributes.worker.js': {
    before: clearEntries,
    // importScripts() does not exist in workerd. The only thing the test needs from
    // webperftestharness.js is the timingAttributes list, so it is defined inline.
    replace: (code): string =>
      code.replace('importScripts("/resources/testharness.js");', '').replace(
        'importScripts("resources/webperftestharness.js");',
        `var timingAttributes = [
            'navigationStart', 'unloadEventStart', 'unloadEventEnd', 'redirectStart',
            'redirectEnd', 'fetchStart', 'domainLookupStart', 'domainLookupEnd',
            'connectStart', 'connectEnd', 'secureConnectionStart', 'requestStart',
            'responseStart', 'responseEnd', 'domLoading', 'domInteractive',
            'domContentLoadedEventStart', 'domContentLoadedEventEnd', 'domComplete',
            'loadEventStart', 'loadEventEnd',
          ];`
      ),
  },
  'mark-entry-constructor.any.js': { before: clearEntries },
  'mark-errors.any.js': { before: clearEntries },
  'mark-l3.any.js': {
    before: clearEntries,
    comment:
      'The test finishes from a PerformanceObserver callback, but PerformanceObserver.observe() is a no-op in workerd, so the test hangs',
    disabledTests: ["mark entries' detail and startTime are customizable."],
  },
  'mark-measure-return-objects.any.js': { before: clearEntries },
  'mark.any.js': { before: clearEntries },
  'measure-l3.any.js': { before: clearEntries },
  'measure-with-dict.any.js': {
    before: clearEntries,
    comment:
      'The test finishes from a PerformanceObserver callback, but PerformanceObserver.observe() is a no-op in workerd, so the test hangs',
    disabledTests: ["measure entries' detail and start/end are customizable"],
  },
  'measure_syntax_err.any.js': { before: clearEntries },
  'performance-measure-invalid.worker.js': {
    before: clearEntries,
    // importScripts() does not exist in workerd, and the test only uses it to load testharness.js.
    replace: (code): string =>
      code.replace('importScripts("/resources/testharness.js");', ''),
  },
  'structured-serialize-detail.any.js': {
    before: clearEntries,
  },
  'supported-usertiming-types.any.js': {
    before: clearEntries,
    comment:
      'supportedEntryTypes lists measure before mark instead of in alphabetical order. The observable tests are disabled because PerformanceObserver.observe() is a no-op, so they hang',
    disabledTests: [
      "'mark' entries should be observable.",
      "'measure' entries should be observable.",
    ],
    expectedFailures: ["supportedEntryTypes contains 'mark' and 'measure'."],
  },
  'user_timing_exists.any.js': { before: clearEntries },
} satisfies TestRunnerConfig;
