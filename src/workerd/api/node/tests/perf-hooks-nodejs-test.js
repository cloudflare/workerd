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
  constants,
} from 'node:perf_hooks';
import { deepStrictEqual, ok, throws } from 'node:assert';

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
    ok(constants);

    // Test that performance is an instance of Performance
    ok(perfHooksPerformance instanceof Performance);
  },
};

export const testPerformanceConstants = {
  test() {
    deepStrictEqual(constants, {
      NODE_PERFORMANCE_GC_FLAGS_ALL_AVAILABLE_GARBAGE: 16,
      NODE_PERFORMANCE_GC_FLAGS_ALL_EXTERNAL_MEMORY: 32,
      NODE_PERFORMANCE_GC_FLAGS_CONSTRUCT_RETAINED: 2,
      NODE_PERFORMANCE_GC_FLAGS_FORCED: 4,
      NODE_PERFORMANCE_GC_FLAGS_NO: 0,
      NODE_PERFORMANCE_GC_FLAGS_SCHEDULE_IDLE: 64,
      NODE_PERFORMANCE_GC_FLAGS_SYNCHRONOUS_PHANTOM_PROCESSING: 8,
      NODE_PERFORMANCE_GC_INCREMENTAL: 8,
      NODE_PERFORMANCE_GC_MAJOR: 4,
      NODE_PERFORMANCE_GC_MINOR: 1,
      NODE_PERFORMANCE_GC_WEAKCB: 16,
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
    throws(() => createHistogram(), { message: /is not implemented/ });
    throws(() => monitorEventLoopDelay(), {
      message: /is not implemented/,
    });
    throws(() => perfHooksPerformance.mark('type-test-mark'), {
      message: /is not implemented/,
    });
    throws(
      () => perfHooksPerformance.measure('type-test-measure', 'type-test-mark'),
      {
        message: /is not implemented/,
      }
    );
  },
};

export const testPerformanceEntryTypes = {
  test() {
    const allEntries = perfHooksPerformance.getEntries();
    ok(Array.isArray(allEntries));

    // Test getting entries by type
    const markEntries = perfHooksPerformance.getEntriesByType('mark');
    ok(Array.isArray(markEntries));

    const measureEntries = perfHooksPerformance.getEntriesByType('measure');
    ok(Array.isArray(measureEntries));

    // The following does not throw
    perfHooksPerformance.clearMarks();
    perfHooksPerformance.clearMeasures();
  },
};

export const testEventCounts = {
  test() {
    // Test that eventCounts exists and is an object
    const eventCounts = perfHooksPerformance.eventCounts;
    ok(eventCounts, 'eventCounts should exist');
    ok(typeof eventCounts === 'object', 'eventCounts should be an object');

    // Test that eventCounts has the expected Map-like methods
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

    // Test that eventCounts has a size property
    ok('size' in eventCounts, 'eventCounts should have size property');
    ok(typeof eventCounts.size === 'number', 'size should be a number');
    ok(eventCounts.size >= 0, 'size should be non-negative');
    deepStrictEqual(
      eventCounts.size,
      0,
      'size should be 0 for empty EventCounts'
    );

    // Test that eventCounts does NOT have mutating methods (it's read-only)
    ok(!('set' in eventCounts), 'eventCounts should not have set method');
    ok(!('delete' in eventCounts), 'eventCounts should not have delete method');
    ok(!('clear' in eventCounts), 'eventCounts should not have clear method');

    // Test Map-like methods behavior with various key types
    // Since we don't track events, these should return empty/false/undefined
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
      ok(
        result === undefined,
        `get('${key}') should return undefined for non-existent keys`
      );

      const hasResult = eventCounts.has(key);
      ok(
        hasResult === false,
        `has('${key}') should return false for non-existent keys`
      );
    }

    // Test that eventCounts is iterable
    ok(
      typeof eventCounts[Symbol.iterator] === 'function',
      'eventCounts should be iterable'
    );

    // Test that Symbol.iterator works correctly (may or may not be the same reference as entries)
    // The spec doesn't require them to be the same reference, just functionally equivalent

    // Test iteration methods return proper iterators
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
    ok(
      entriesResult.done === true,
      'entries iterator should be done immediately (empty map)'
    );
    ok(
      entriesResult.value === undefined,
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
    ok(
      keysResult.done === true,
      'keys iterator should be done immediately (empty map)'
    );
    ok(
      keysResult.value === undefined,
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
    ok(
      valuesResult.done === true,
      'values iterator should be done immediately (empty map)'
    );
    ok(
      valuesResult.value === undefined,
      'values iterator value should be undefined when done'
    );

    // Test forEach with different scenarios
    let forEachCalled = false;
    let forEachContext = null;
    const customThis = { custom: true };

    eventCounts.forEach(function () {
      forEachCalled = true;
    });
    // Since the map is empty, forEach shouldn't call the callback
    ok(!forEachCalled, 'forEach should not call callback when map is empty');

    // Test forEach with thisArg
    eventCounts.forEach(function () {
      forEachContext = this;
    }, customThis);
    ok(
      forEachContext === null,
      'forEach with thisArg should not be called for empty map'
    );

    // Test that we can iterate over it with for...of
    const entries = [];
    for (const entry of eventCounts) {
      entries.push(entry);
    }
    ok(Array.isArray(entries), 'Should be able to iterate with for...of');
    ok(entries.length === 0, 'Should have no entries currently');

    // Test spread operator
    const spreadEntries = [...eventCounts];
    ok(Array.isArray(spreadEntries), 'Should be able to use spread operator');
    ok(spreadEntries.length === 0, 'Spread should produce empty array');

    const spreadKeys = [...eventCounts.keys()];
    ok(Array.isArray(spreadKeys), 'Should be able to spread keys');
    ok(spreadKeys.length === 0, 'Keys spread should produce empty array');

    const spreadValues = [...eventCounts.values()];
    ok(Array.isArray(spreadValues), 'Should be able to spread values');
    ok(spreadValues.length === 0, 'Values spread should produce empty array');

    // Test Array.from
    const fromEntries = Array.from(eventCounts);
    ok(Array.isArray(fromEntries), 'Array.from should work with eventCounts');
    ok(fromEntries.length === 0, 'Array.from should produce empty array');

    // Verify eventCounts reference behavior
    const eventCounts2 = perfHooksPerformance.eventCounts;
    ok(
      eventCounts2,
      'Second call to performance.eventCounts should also return an object'
    );
    // Note: The implementation may return a new instance each time, which is acceptable
  },
};
