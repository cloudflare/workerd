import {
  performance as perfHooksPerformance,
  Performance,
  PerformanceEntry,
  PerformanceMark,
  PerformanceMeasure,
  PerformanceObserver,
  PerformanceObserverEntryList,
  PerformanceResourceTiming,
  createHistogram,
  monitorEventLoopDelay,
  eventLoopUtilization,
  timerify,
  constants,
} from 'node:perf_hooks';
import {
  deepStrictEqual,
  doesNotThrow,
  ok,
  throws,
  strictEqual,
  notStrictEqual,
} from 'node:assert';

export const testPerformanceExports = {
  test() {
    // Test that all expected exports exist
    ok(performance);
    ok(Performance);
    ok(PerformanceEntry);
    ok(PerformanceMark);
    ok(PerformanceMeasure);
    ok(PerformanceObserver);
    ok(PerformanceObserverEntryList);
    ok(PerformanceResourceTiming);
    ok(createHistogram);
    ok(monitorEventLoopDelay);
    ok(eventLoopUtilization);
    ok(timerify);
    ok(constants);

    // Test that performance is an instance of Performance
    ok(perfHooksPerformance instanceof Performance);
  },
};

export const testPerformanceConstants = {
  test() {
    deepStrictEqual(constants, {
      // GC type constants
      NODE_PERFORMANCE_GC_MAJOR: 4,
      NODE_PERFORMANCE_GC_MINOR: 1,
      NODE_PERFORMANCE_GC_INCREMENTAL: 8,
      NODE_PERFORMANCE_GC_WEAKCB: 16,

      // GC flags constants
      NODE_PERFORMANCE_GC_FLAGS_NO: 0,
      NODE_PERFORMANCE_GC_FLAGS_CONSTRUCT_RETAINED: 2,
      NODE_PERFORMANCE_GC_FLAGS_FORCED: 4,
      NODE_PERFORMANCE_GC_FLAGS_SYNCHRONOUS_PHANTOM_PROCESSING: 8,
      NODE_PERFORMANCE_GC_FLAGS_ALL_AVAILABLE_GARBAGE: 16,
      NODE_PERFORMANCE_GC_FLAGS_ALL_EXTERNAL_MEMORY: 32,
      NODE_PERFORMANCE_GC_FLAGS_SCHEDULE_IDLE: 64,

      // Entry type constants
      NODE_PERFORMANCE_ENTRY_TYPE_GC: 0,
      NODE_PERFORMANCE_ENTRY_TYPE_HTTP: 1,
      NODE_PERFORMANCE_ENTRY_TYPE_HTTP2: 2,
      NODE_PERFORMANCE_ENTRY_TYPE_NET: 3,
      NODE_PERFORMANCE_ENTRY_TYPE_DNS: 4,

      // Milestone constants
      NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN_TIMESTAMP: 0,
      NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN: 1,
      NODE_PERFORMANCE_MILESTONE_ENVIRONMENT: 2,
      NODE_PERFORMANCE_MILESTONE_NODE_START: 3,
      NODE_PERFORMANCE_MILESTONE_V8_START: 4,
      NODE_PERFORMANCE_MILESTONE_LOOP_START: 5,
      NODE_PERFORMANCE_MILESTONE_LOOP_EXIT: 6,
      NODE_PERFORMANCE_MILESTONE_BOOTSTRAP_COMPLETE: 7,
    });
    ok(Object.isFrozen(constants));
  },
};

export const testPerformanceBasicFunctionality = {
  test() {
    // Test that performance.now() works
    const start = perfHooksPerformance.now();
    ok(typeof start === 'number');
    ok(start >= 0, 'start should be bigger than or equal to 0');

    // Test that consecutive calls return increasing values
    const end = perfHooksPerformance.now();
    ok(end >= start, 'end should be bigger than or equal to start');

    // Test performance.timeOrigin
    ok(typeof perfHooksPerformance.timeOrigin === 'number');
    ok(
      perfHooksPerformance.timeOrigin >= 0,
      'timeOrigin should be bigger than or equal to 0'
    );
  },
};

export const testUnimplementedMethods = {
  test() {
    // These methods throw because they cannot be meaningfully stubbed
    throws(() => createHistogram(), { message: /is not implemented/ });
    throws(() => monitorEventLoopDelay(), {
      message: /is not implemented/,
    });
  },
};

export const testStandaloneEventLoopUtilization = {
  test() {
    // eventLoopUtilization returns stub values for compatibility
    const result = eventLoopUtilization();
    ok(typeof result === 'object', 'should return an object');
    strictEqual(result.idle, 0, 'idle should be 0');
    strictEqual(result.active, 0, 'active should be 0');
    strictEqual(result.utilization, 0, 'utilization should be 0');

    // Should accept optional parameters without error
    const result2 = eventLoopUtilization(result);
    ok(
      typeof result2 === 'object',
      'should return an object with one argument'
    );

    const result3 = eventLoopUtilization(result, result2);
    ok(
      typeof result3 === 'object',
      'should return an object with two arguments'
    );
  },
};

export const testStandaloneTimerify = {
  test() {
    // timerify returns the function as-is (stub behavior)
    const testFn = () => 42;
    const timerified = timerify(testFn);

    strictEqual(timerified, testFn, 'timerify should return the same function');
    strictEqual(timerified(), 42, 'timerified function should work correctly');
  },
};

export const testPerformanceNodeTiming = {
  test() {
    // nodeTiming is a Node.js-specific property available on both
    // the perf_hooks performance export and globalThis.performance
    ok(
      perfHooksPerformance.nodeTiming,
      'nodeTiming should exist on perf_hooks'
    );
    ok(
      globalThis.performance.nodeTiming,
      'nodeTiming should exist on globalThis'
    );

    const nodeTiming = perfHooksPerformance.nodeTiming;
    strictEqual(nodeTiming.name, 'node', 'name should be "node"');
    strictEqual(nodeTiming.entryType, 'node', 'entryType should be "node"');
    strictEqual(
      typeof nodeTiming.startTime,
      'number',
      'startTime should be a number'
    );
    strictEqual(
      typeof nodeTiming.duration,
      'number',
      'duration should be a number'
    );
    strictEqual(
      typeof nodeTiming.nodeStart,
      'number',
      'nodeStart should be a number'
    );
    strictEqual(
      typeof nodeTiming.v8Start,
      'number',
      'v8Start should be a number'
    );
    strictEqual(
      typeof nodeTiming.bootstrapComplete,
      'number',
      'bootstrapComplete should be a number'
    );
    strictEqual(
      typeof nodeTiming.environment,
      'number',
      'environment should be a number'
    );
    strictEqual(
      typeof nodeTiming.loopStart,
      'number',
      'loopStart should be a number'
    );
    strictEqual(
      typeof nodeTiming.loopExit,
      'number',
      'loopExit should be a number'
    );
    strictEqual(
      typeof nodeTiming.idleTime,
      'number',
      'idleTime should be a number'
    );

    // uvMetricsInfo should return an object with libuv metrics (stub values)
    ok(
      typeof nodeTiming.uvMetricsInfo === 'object',
      'uvMetricsInfo should be an object'
    );
    strictEqual(
      nodeTiming.uvMetricsInfo.loopCount,
      0,
      'uvMetricsInfo.loopCount should be 0'
    );
    strictEqual(
      nodeTiming.uvMetricsInfo.events,
      0,
      'uvMetricsInfo.events should be 0'
    );
    strictEqual(
      nodeTiming.uvMetricsInfo.eventsWaiting,
      0,
      'uvMetricsInfo.eventsWaiting should be 0'
    );

    // Verify that nodeTiming properties are instance (own) properties, not prototype properties
    // This matches Node.js behavior where Reflect.ownKeys(performance.nodeTiming) includes
    // all properties like nodeStart, v8Start, etc.
    const ownKeys = Reflect.ownKeys(nodeTiming);
    ok(ownKeys.includes('nodeStart'), 'nodeStart should be an own property');
    ok(ownKeys.includes('v8Start'), 'v8Start should be an own property');
    ok(
      ownKeys.includes('bootstrapComplete'),
      'bootstrapComplete should be an own property'
    );
    ok(
      ownKeys.includes('environment'),
      'environment should be an own property'
    );
    ok(ownKeys.includes('loopStart'), 'loopStart should be an own property');
    ok(ownKeys.includes('loopExit'), 'loopExit should be an own property');
    ok(ownKeys.includes('idleTime'), 'idleTime should be an own property');
    ok(
      ownKeys.includes('uvMetricsInfo'),
      'uvMetricsInfo should be an own property'
    );

    // Verify prototype only has constructor and toJSON (matching Node.js behavior)
    const protoKeys = Reflect.ownKeys(Object.getPrototypeOf(nodeTiming));
    ok(protoKeys.includes('constructor'), 'prototype should have constructor');
    ok(protoKeys.includes('toJSON'), 'prototype should have toJSON');

    // toJSON should work and include uvMetricsInfo
    ok(typeof nodeTiming.toJSON === 'function', 'toJSON should be a function');
    const json = nodeTiming.toJSON();
    ok(typeof json === 'object', 'toJSON should return an object');
    ok(
      typeof json.uvMetricsInfo === 'object',
      'toJSON should include uvMetricsInfo'
    );
  },
};

export const testPerformanceEventLoopUtilization = {
  test() {
    // performance.eventLoopUtilization() should return stub values (not throw)
    const result = perfHooksPerformance.eventLoopUtilization();
    ok(typeof result === 'object', 'should return an object');
    strictEqual(result.idle, 0, 'idle should be 0');
    strictEqual(result.active, 0, 'active should be 0');
    strictEqual(result.utilization, 0, 'utilization should be 0');
  },
};

export const testPerformanceTimerify = {
  test() {
    // performance.timerify() should return the function as-is (stub behavior)
    const testFn = () => 'test';
    const timerified = perfHooksPerformance.timerify(testFn);

    strictEqual(timerified, testFn, 'timerify should return the same function');
    strictEqual(
      timerified(),
      'test',
      'timerified function should work correctly'
    );
  },
};

export const testPerformanceMarkResourceTiming = {
  test() {
    // markResourceTiming should not throw (no-op stub)
    perfHooksPerformance.markResourceTiming();
    // If we get here without throwing, the test passes
    ok(true, 'markResourceTiming should not throw');
  },
};

export const testPerformanceMark = {
  test() {
    perfHooksPerformance.clearMarks();

    const mark1 = perfHooksPerformance.mark('test-mark-1');
    ok(
      mark1 instanceof PerformanceMark,
      'mark should return a PerformanceMark instance'
    );
    strictEqual(mark1.name, 'test-mark-1', 'mark name should match');
    strictEqual(mark1.entryType, 'mark', 'entryType should be "mark"');
    ok(typeof mark1.startTime === 'number', 'startTime should be a number');
    strictEqual(mark1.duration, 0, 'duration should be 0 for marks');

    const customTime = 100;
    const mark2 = perfHooksPerformance.mark('test-mark-2', {
      startTime: customTime,
    });
    strictEqual(mark2.startTime, customTime, 'custom startTime should be set');

    const detail = { key: 'value' };
    const mark3 = perfHooksPerformance.mark('test-mark-3', { detail });
    ok(mark3.detail, 'detail should be set');
    deepStrictEqual(mark3.detail, detail, 'detail should match');

    const entries = perfHooksPerformance.getEntries();
    ok(entries.length >= 3, 'should have at least 3 entries');

    const marks = perfHooksPerformance.getEntriesByType('mark');
    ok(marks.length >= 3, 'should have at least 3 marks');

    perfHooksPerformance.clearMarks();
  },
};

export const testPerformanceMeasure = {
  test() {
    perfHooksPerformance.clearMarks();
    perfHooksPerformance.clearMeasures();

    const mark1 = perfHooksPerformance.mark('start-mark');
    const mark2 = perfHooksPerformance.mark('end-mark');

    const measure1 = perfHooksPerformance.measure(
      'test-measure-1',
      'start-mark',
      'end-mark'
    );
    ok(
      measure1 instanceof PerformanceMeasure,
      'measure should return a PerformanceMeasure instance'
    );
    strictEqual(measure1.name, 'test-measure-1', 'measure name should match');
    strictEqual(measure1.entryType, 'measure', 'entryType should be "measure"');
    ok(typeof measure1.startTime === 'number', 'startTime should be a number');
    ok(typeof measure1.duration === 'number', 'duration should be a number');
    strictEqual(
      measure1.startTime,
      mark1.startTime,
      'measure startTime should match start mark'
    );
    strictEqual(
      measure1.detail,
      null,
      'detail should be null when using string marks'
    );

    const measure2 = perfHooksPerformance.measure('test-measure-2', {
      start: 50,
      end: 150,
    });
    strictEqual(
      measure2.startTime,
      50,
      'measure with options should have correct startTime'
    );
    strictEqual(
      measure2.duration,
      100,
      'measure with options should have correct duration'
    );
    strictEqual(
      measure2.detail,
      null,
      'detail should be null when not provided in options'
    );

    const measure3 = perfHooksPerformance.measure('test-measure-3', {
      start: 100,
      duration: 50,
    });
    strictEqual(
      measure3.startTime,
      100,
      'measure with duration should have correct startTime'
    );
    strictEqual(
      measure3.duration,
      50,
      'measure with duration should have correct duration'
    );
    strictEqual(
      measure3.detail,
      null,
      'detail should be null when not provided in options'
    );

    const customDetail = { customKey: 'customValue' };
    const measure4 = perfHooksPerformance.measure('test-measure-4', {
      start: 200,
      end: 300,
      detail: customDetail,
    });
    ok(measure4.detail, 'measure should have detail');
    deepStrictEqual(measure4.detail, customDetail, 'detail should match');

    const measures = perfHooksPerformance.getEntriesByType('measure');
    ok(measures.length >= 4, 'should have at least 4 measures');

    perfHooksPerformance.clearMarks();
    perfHooksPerformance.clearMeasures();
  },
};

export const testPerformanceMeasureNameOnly = {
  test() {
    perfHooksPerformance.clearMarks();
    perfHooksPerformance.clearMeasures();

    const before = perfHooksPerformance.now();
    const measure = perfHooksPerformance.measure('name-only-measure');
    const after = perfHooksPerformance.now();

    ok(
      measure instanceof PerformanceMeasure,
      'measure should return a PerformanceMeasure instance'
    );
    strictEqual(measure.name, 'name-only-measure', 'measure name should match');
    strictEqual(measure.entryType, 'measure', 'entryType should be "measure"');
    strictEqual(measure.startTime, 0, 'startTime should be 0 (timeOrigin)');
    ok(
      measure.duration >= before && measure.duration <= after,
      'duration should be approximately the current time'
    );
    strictEqual(
      measure.detail,
      null,
      'detail should be null when not provided'
    );

    perfHooksPerformance.clearMeasures();
  },
};

export const testGetEntriesByName = {
  test() {
    perfHooksPerformance.clearMarks();
    perfHooksPerformance.clearMeasures();

    perfHooksPerformance.mark('test-name');
    perfHooksPerformance.mark('test-name');
    perfHooksPerformance.mark('other-name');
    perfHooksPerformance.measure('test-name', { start: 0, end: 100 });

    const entriesByName = perfHooksPerformance.getEntriesByName('test-name');
    ok(Array.isArray(entriesByName), 'getEntriesByName should return an array');
    strictEqual(
      entriesByName.length,
      3,
      'should have 3 entries with name "test-name"'
    );

    const marksByName = perfHooksPerformance.getEntriesByName(
      'test-name',
      'mark'
    );
    strictEqual(
      marksByName.length,
      2,
      'should have 2 marks with name "test-name"'
    );

    const measuresByName = perfHooksPerformance.getEntriesByName(
      'test-name',
      'measure'
    );
    strictEqual(
      measuresByName.length,
      1,
      'should have 1 measure with name "test-name"'
    );

    const noEntries = perfHooksPerformance.getEntriesByName('non-existent');
    ok(
      Array.isArray(noEntries),
      'should return empty array for non-existent name'
    );
    strictEqual(noEntries.length, 0, 'should have 0 entries');

    perfHooksPerformance.clearMarks();
    perfHooksPerformance.clearMeasures();
  },
};

export const testClearMethods = {
  test() {
    perfHooksPerformance.mark('mark-to-keep');
    perfHooksPerformance.mark('mark-to-remove');
    perfHooksPerformance.mark('mark-to-remove');
    perfHooksPerformance.mark('another-mark');

    perfHooksPerformance.clearMarks('mark-to-remove');
    const marksAfterClear = perfHooksPerformance.getEntriesByType('mark');
    strictEqual(
      marksAfterClear.length,
      2,
      'should have 2 marks after clearing specific name'
    );
    ok(
      marksAfterClear.every((m) => m.name !== 'mark-to-remove'),
      'should not have removed marks'
    );
    ok(
      marksAfterClear.some((m) => m.name === 'mark-to-keep'),
      'should have kept other marks'
    );

    perfHooksPerformance.clearMarks();
    const allMarksCleared = perfHooksPerformance.getEntriesByType('mark');
    strictEqual(
      allMarksCleared.length,
      0,
      'should have 0 marks after clearing all'
    );

    perfHooksPerformance.measure('measure-to-keep', { start: 0, end: 100 });
    perfHooksPerformance.measure('measure-to-remove', { start: 100, end: 200 });
    perfHooksPerformance.measure('measure-to-remove', { start: 200, end: 300 });

    perfHooksPerformance.clearMeasures('measure-to-remove');
    const measuresAfterClear = perfHooksPerformance.getEntriesByType('measure');
    strictEqual(
      measuresAfterClear.length,
      1,
      'should have 1 measure after clearing specific name'
    );
    strictEqual(
      measuresAfterClear[0].name,
      'measure-to-keep',
      'should have kept the right measure'
    );

    perfHooksPerformance.clearMeasures();
    const allMeasuresCleared = perfHooksPerformance.getEntriesByType('measure');
    strictEqual(
      allMeasuresCleared.length,
      0,
      'should have 0 measures after clearing all'
    );

    perfHooksPerformance.clearResourceTimings();
    const resourceTimings = perfHooksPerformance.getEntriesByType('resource');
    strictEqual(resourceTimings.length, 0, 'should have 0 resource timings');
  },
};

export const testPerformanceEntryTypes = {
  test() {
    const allEntries = perfHooksPerformance.getEntries();
    ok(Array.isArray(allEntries));

    const markEntries = perfHooksPerformance.getEntriesByType('mark');
    ok(Array.isArray(markEntries));

    const measureEntries = perfHooksPerformance.getEntriesByType('measure');
    ok(Array.isArray(measureEntries));

    perfHooksPerformance.clearMarks();
    perfHooksPerformance.clearMeasures();
    perfHooksPerformance.clearResourceTimings();
  },
};

export const testEventCounts = {
  test() {
    const eventCounts = perfHooksPerformance.eventCounts;
    ok(eventCounts, 'eventCounts should exist');
    ok(typeof eventCounts === 'object', 'eventCounts should be an object');

    ok(
      typeof eventCounts.get === 'function',
      'eventCounts should have get method'
    );
    ok(
      typeof eventCounts.has === 'function',
      'eventCounts should have has method'
    );
    ok(
      typeof eventCounts.entries === 'function',
      'eventCounts should have entries method'
    );
    ok(
      typeof eventCounts.keys === 'function',
      'eventCounts should have keys method'
    );
    ok(
      typeof eventCounts.values === 'function',
      'eventCounts should have values method'
    );
    ok(
      typeof eventCounts.forEach === 'function',
      'eventCounts should have forEach method'
    );

    ok('size' in eventCounts, 'eventCounts should have size property');
    ok(typeof eventCounts.size === 'number', 'size should be a number');
    ok(eventCounts.size >= 0, 'size should be non-negative');
    strictEqual(eventCounts.size, 0, 'size should be 0 for empty EventCounts');

    ok(!('set' in eventCounts), 'eventCounts should not have set method');
    ok(!('delete' in eventCounts), 'eventCounts should not have delete method');
    ok(!('clear' in eventCounts), 'eventCounts should not have clear method');

    const testKeys = [
      'click',
      'keydown',
      '',
      'special-chars!@#',
      '123',
      'undefined',
      'null',
    ];
    for (const key of testKeys) {
      const result = eventCounts.get(key);
      strictEqual(
        result,
        undefined,
        `get('${key}') should return undefined for non-existent keys`
      );

      const hasResult = eventCounts.has(key);
      strictEqual(
        hasResult,
        false,
        `has('${key}') should return false for non-existent keys`
      );
    }

    ok(
      typeof eventCounts[Symbol.iterator] === 'function',
      'eventCounts should be iterable'
    );

    const entriesIter = eventCounts.entries();
    ok(
      entriesIter && typeof entriesIter.next === 'function',
      'entries() should return an iterator'
    );
    ok(
      typeof entriesIter[Symbol.iterator] === 'function',
      'entries iterator should be iterable'
    );
    const entriesResult = entriesIter.next();
    strictEqual(
      entriesResult.done,
      true,
      'entries iterator should be done immediately (empty map)'
    );
    strictEqual(
      entriesResult.value,
      undefined,
      'entries iterator value should be undefined when done'
    );

    const keysIter = eventCounts.keys();
    ok(
      keysIter && typeof keysIter.next === 'function',
      'keys() should return an iterator'
    );
    ok(
      typeof keysIter[Symbol.iterator] === 'function',
      'keys iterator should be iterable'
    );
    const keysResult = keysIter.next();
    strictEqual(
      keysResult.done,
      true,
      'keys iterator should be done immediately (empty map)'
    );
    strictEqual(
      keysResult.value,
      undefined,
      'keys iterator value should be undefined when done'
    );

    const valuesIter = eventCounts.values();
    ok(
      valuesIter && typeof valuesIter.next === 'function',
      'values() should return an iterator'
    );
    ok(
      typeof valuesIter[Symbol.iterator] === 'function',
      'values iterator should be iterable'
    );
    const valuesResult = valuesIter.next();
    strictEqual(
      valuesResult.done,
      true,
      'values iterator should be done immediately (empty map)'
    );
    strictEqual(
      valuesResult.value,
      undefined,
      'values iterator value should be undefined when done'
    );

    let forEachCalled = false;
    let forEachContext = null;
    const customThis = { custom: true };

    eventCounts.forEach(function () {
      forEachCalled = true;
    });
    ok(!forEachCalled, 'forEach should not call callback when map is empty');

    eventCounts.forEach(function () {
      forEachContext = this;
    }, customThis);
    strictEqual(
      forEachContext,
      null,
      'forEach with thisArg should not be called for empty map'
    );

    const entries = [];
    for (const entry of eventCounts) {
      entries.push(entry);
    }
    ok(Array.isArray(entries), 'Should be able to iterate with for...of');
    strictEqual(entries.length, 0, 'Should have no entries currently');

    const spreadEntries = [...eventCounts];
    ok(Array.isArray(spreadEntries), 'Should be able to use spread operator');
    strictEqual(spreadEntries.length, 0, 'Spread should produce empty array');

    const spreadKeys = [...eventCounts.keys()];
    ok(Array.isArray(spreadKeys), 'Should be able to spread keys');
    strictEqual(spreadKeys.length, 0, 'Keys spread should produce empty array');

    const spreadValues = [...eventCounts.values()];
    ok(Array.isArray(spreadValues), 'Should be able to spread values');
    strictEqual(
      spreadValues.length,
      0,
      'Values spread should produce empty array'
    );

    const fromEntries = Array.from(eventCounts);
    ok(Array.isArray(fromEntries), 'Array.from should work with eventCounts');
    strictEqual(fromEntries.length, 0, 'Array.from should produce empty array');

    const eventCounts2 = perfHooksPerformance.eventCounts;
    ok(
      eventCounts2,
      'Second call to performance.eventCounts should also return an object'
    );
  },
};

export const testPerformanceMarkToJSON = {
  test() {
    const mark1 = perfHooksPerformance.mark('test-mark-json');
    const json1 = mark1.toJSON();
    strictEqual(json1.name, 'test-mark-json');
    strictEqual(json1.entryType, 'mark');
    strictEqual(json1.duration, 0);
    ok(!('detail' in json1));

    const detail = { key: 'value' };
    const mark2 = perfHooksPerformance.mark('test-mark-json-2', { detail });
    const json2 = mark2.toJSON();
    deepStrictEqual(json2.detail, detail);
  },
};

export const testPerformanceMeasureToJSON = {
  test() {
    perfHooksPerformance.mark('start');
    perfHooksPerformance.mark('end');
    const measure1 = perfHooksPerformance.measure('test-json', 'start', 'end');
    const json1 = measure1.toJSON();
    strictEqual(json1.name, 'test-json');
    strictEqual(json1.entryType, 'measure');
    // detail should not be in JSON when not provided (it's null)
    ok(!('detail' in json1), 'detail should not be in JSON when not provided');

    const customDetail = { value: 42 };
    const measure2 = perfHooksPerformance.measure('test-json-2', {
      start: 50,
      end: 150,
      detail: customDetail,
    });
    const json2 = measure2.toJSON();
    deepStrictEqual(json2.detail, customDetail);
  },
};

export const testPerformanceToJSON = {
  test() {
    const json = perfHooksPerformance.toJSON();
    strictEqual(typeof json, 'object');
    strictEqual(json.timeOrigin, 0);
    // toJSON should not include other properties beyond timeOrigin
    deepStrictEqual(Object.keys(json), ['timeOrigin']);
  },
};

export const testSetResourceTimingBufferSize = {
  test() {
    doesNotThrow(() => perfHooksPerformance.setResourceTimingBufferSize(0));
  },
};

export const testGlobalThis = {
  async test() {
    notStrictEqual(typeof globalThis.performance, 'undefined');
    ok('addEventListener' in globalThis.performance);
    notStrictEqual(typeof globalThis.Performance, 'undefined');
  },
};
