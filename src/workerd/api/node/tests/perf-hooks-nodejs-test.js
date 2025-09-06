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
    throws(() => createHistogram(), { code: 'ERR_METHOD_NOT_IMPLEMENTED' });
    throws(() => monitorEventLoopDelay(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
    throws(() => perfHooksPerformance.mark('type-test-mark'), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
    throws(
      () => perfHooksPerformance.measure('type-test-measure', 'type-test-mark'),
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
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
