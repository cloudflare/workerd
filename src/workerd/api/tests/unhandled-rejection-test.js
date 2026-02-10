// Regression tests for https://github.com/cloudflare/workerd/issues/6020
// Unhandled rejection should NOT fire for promises that are handled through
// multi-tick promise chains.

import { strictEqual, ok, rejects } from 'node:assert';
import { mock } from 'node:test';

const asyncFunction = async (name) => {
  throw new Error(`this function rejects: ${name}`);
};

// Verifies assert.rejects handles rejections without unhandledrejection.
export const assertRejects = {
  async test() {
    const handler = mock.fn();
    addEventListener('unhandledrejection', handler);
    try {
      await rejects(async () => asyncFunction('A'));
      strictEqual(
        handler.mock.callCount(),
        0,
        'unhandledrejection should not fire for assert.rejects'
      );
    } finally {
      removeEventListener('unhandledrejection', handler);
    }
  },
};

// Verifies chained .then().catch() handling avoids unhandledrejection.
export const promiseChainCatch = {
  async test() {
    const handler = mock.fn();
    addEventListener('unhandledrejection', handler);
    try {
      const error = await Promise.resolve()
        .then(() => asyncFunction('B'))
        .then(() => null)
        .catch((e) => e);
      ok(error instanceof Error);
      strictEqual(error.message, 'this function rejects: B');
      strictEqual(
        handler.mock.callCount(),
        0,
        'unhandledrejection should not fire for .catch() chain'
      );
    } finally {
      removeEventListener('unhandledrejection', handler);
    }
  },
};

// Verifies try/catch around awaited chain avoids unhandledrejection.
export const tryCatchAwait = {
  async test() {
    const handler = mock.fn();
    addEventListener('unhandledrejection', handler);
    try {
      try {
        await Promise.resolve('C').then(asyncFunction);
      } catch (error) {
        ok(error instanceof Error);
        strictEqual(error.message, 'this function rejects: C');
      }
      strictEqual(
        handler.mock.callCount(),
        0,
        'unhandledrejection should not fire for try/catch'
      );
    } finally {
      removeEventListener('unhandledrejection', handler);
    }
  },
};

// Verifies a truly unhandled rejection still emits unhandledrejection.
export const genuineUnhandledRejectionStillFires = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const handler = mock.fn(() => resolve());
    addEventListener('unhandledrejection', handler, { once: true });
    Promise.reject('boom');
    await promise;
    strictEqual(
      handler.mock.callCount(),
      1,
      'unhandledrejection should fire for genuinely unhandled rejection'
    );
  },
};

// Verifies unhandledrejection followed by rejectionhandled on late catch.
export const lateHandlerTriggersRejectionhandled = {
  async test() {
    const { promise: unhandledPromise, resolve: resolveUnhandled } =
      Promise.withResolvers();
    const { promise: handledPromise, resolve: resolveHandled } =
      Promise.withResolvers();
    let unhandledReason;
    let handledReason;
    const unhandledHandler = mock.fn((event) => {
      unhandledReason = event.reason;
      resolveUnhandled();
    });
    const handledHandler = mock.fn((event) => {
      handledReason = event.reason;
      resolveHandled();
    });
    addEventListener('unhandledrejection', unhandledHandler, { once: true });
    addEventListener('rejectionhandled', handledHandler, { once: true });
    try {
      const error = new Error('late');
      const promise = Promise.reject(error);
      await unhandledPromise;
      promise.catch(() => {});
      await handledPromise;
      strictEqual(
        unhandledHandler.mock.callCount(),
        1,
        'unhandledrejection should fire once before late handler'
      );
      strictEqual(
        handledHandler.mock.callCount(),
        1,
        'rejectionhandled should fire after late handler'
      );
      ok(unhandledReason instanceof Error);
      strictEqual(unhandledReason.message, 'late');
      strictEqual(
        handledReason,
        undefined,
        'rejectionhandled reason should be undefined'
      );
    } finally {
      removeEventListener('unhandledrejection', unhandledHandler);
      removeEventListener('rejectionhandled', handledHandler);
    }
  },
};

// Verifies unhandledrejection handler can trigger another unhandled rejection.
export const handlerTriggeredUnhandledRejection = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const timeout = new Promise((resolveTimeout) => {
      setTimeout(resolveTimeout, 25);
    });
    const reasons = [];
    let callCount = 0;
    const handler = mock.fn((event) => {
      reasons.push(event.reason);
      callCount += 1;
      if (callCount === 1) {
        queueMicrotask(() => Promise.reject(new Error('second')));
      }
      if (callCount === 2) {
        resolve();
      }
    });
    addEventListener('unhandledrejection', handler);
    try {
      Promise.reject(new Error('first'));
      await Promise.race([promise, timeout]);
      strictEqual(
        handler.mock.callCount(),
        2,
        'unhandledrejection should fire for rejection triggered by handler'
      );
      strictEqual(reasons.length, 2);
      ok(reasons[0] instanceof Error);
      strictEqual(reasons[0].message, 'first');
      ok(reasons[1] instanceof Error);
      strictEqual(reasons[1].message, 'second');
    } finally {
      removeEventListener('unhandledrejection', handler);
    }
  },
};

// Verifies each unhandled rejection emits its own event.
export const multipleUnhandledRejections = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const timeout = new Promise((resolveTimeout) => {
      setTimeout(resolveTimeout, 25);
    });
    const handler = mock.fn(() => {
      if (handler.mock.callCount() === 2) {
        resolve();
      }
    });
    addEventListener('unhandledrejection', handler);
    try {
      Promise.reject(new Error('one'));
      Promise.reject(new Error('two'));
      await Promise.race([promise, timeout]);
      strictEqual(
        handler.mock.callCount(),
        2,
        'unhandledrejection should fire for each unhandled rejection'
      );
    } finally {
      removeEventListener('unhandledrejection', handler);
    }
  },
};
