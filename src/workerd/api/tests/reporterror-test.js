import { mock } from 'node:test';
import { strictEqual, throws } from 'node:assert';

const boom = new Error('boom');

const handler = mock.fn((event) => {
  if (event.error instanceof Error) {
    strictEqual(event.message, 'Uncaught Error: boom');
    strictEqual(event.colno, 13);
    strictEqual(event.lineno, 4);
    strictEqual(event.filename, 'worker');
    strictEqual(event.error, boom);
  } else {
    strictEqual(event.message, 'Uncaught boom');
    strictEqual(event.colno, 0);
    strictEqual(event.lineno, 25);
    strictEqual(event.filename, 'worker');
    strictEqual(event.error, 'boom');
  }
  return true;
});

addEventListener('error', handler);

reportError('boom');

throws(() => reportError(), {
  message:
    "Failed to execute 'reportError' on 'ServiceWorkerGlobalScope': " +
    "parameter 1 is not of type 'JsValue'.",
});

export const reportErrorTest = {
  test() {
    // TODO(soon): We are limited in what we can test here because we cannot
    // inspect the log output and workerd does not implement that WorkerTracer
    // used for collecting data for tail workers. The best we can currently do
    // is make sure the basic API is working and that the mock fn was called.
    reportError(boom);
    strictEqual(handler.mock.calls.length, 2);
  },
};
