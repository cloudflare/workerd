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
  'mark-errors.any.js': {
    before: clearEntries,
    comment: 'The PerformanceMark constructor accepts a negative startTime',
    expectedFailures: [
      '[new PerformanceMark]: Negative startTime in mark-options should be rejected',
    ],
  },
  'mark-l3.any.js': {
    before: clearEntries,
    comment:
      'mark() rejects a detail that is not an object (here null), because the detail option is typed as an object instead of any',
    expectedFailures: ["mark entries' detail and startTime are customizable."],
  },
  'mark-measure-return-objects.any.js': { before: clearEntries },
  'mark.any.js': { before: clearEntries },
  'measure-l3.any.js': {
    before: clearEntries,
    comment:
      'measure() ignores the end mark when the start mark is undefined, and measures up to now instead',
    expectedFailures: [
      "When the end mark is given and the start is unprovided, the end time of the measure entry should be the end mark's time, the start time should be 0.",
    ],
  },
  'measure-with-dict.any.js': {
    before: clearEntries,
    comment:
      'measure() rejects a detail that is not an object, and does not throw a TypeError for an options object combined with an end mark or for a negative start or end',
    expectedFailures: [
      "measure entries' detail and start/end are customizable",
      'measure should throw a TypeError when passed an invalid argument combination',
    ],
  },
  'measure_syntax_err.any.js': {
    before: clearEntries,
    comment:
      'measure() does not throw a SyntaxError when a mark does not exist; it uses a default time instead',
    expectedFailures: [
      'self.performance.measure("measure", "mark"), where "mark" is a non-existent mark, throws a SyntaxError exception.',
      'self.performance.measure("measure", "mark", "existing_mark"), where "mark" is a non-existent mark, throws a SyntaxError exception.',
      'self.performance.measure("measure", "existing_mark", "mark"), where "mark" is a non-existent mark, throws a SyntaxError exception.',
      'self.performance.measure("measure", "mark", "mark"), where "mark" is a non-existent mark, throws a SyntaxError exception.',
    ],
  },
  'performance-measure-invalid.worker.js': {
    before: clearEntries,
    comment:
      'measure() does not throw a TypeError for PerformanceTiming attribute names such as navigationStart outside a Window',
    expectedFailures: [
      "When converting 'navigationStart' to a timestamp, the global object has to be a Window object.",
      "When converting 'navigationStart' to a timestamp and a mark named 'navigationStart' exists, the global object has to be a Window object.",
    ],
    // importScripts() does not exist in workerd, and the test only uses it to load testharness.js.
    replace: (code): string =>
      code.replace('importScripts("/resources/testharness.js");', ''),
  },
  'structured-serialize-detail.any.js': {
    before: clearEntries,
    comment:
      'mark() and measure() store detail by reference instead of structured-cloning it, so the entry shares the caller object and a Symbol is not rejected',
    expectedFailures: [
      'The detail property in the mark constructor should be structured-clone.',
      'The detail property in the mark method should be structured-clone.',
      'Mark: Throw an exception when the detail property cannot be structured-serialized.',
      'The detail property in the measure method should be structured-clone.',
      'Measure: Throw an exception when the detail property cannot be structured-serialized.',
      'The detail object is cloned when passed to mark API.',
    ],
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
