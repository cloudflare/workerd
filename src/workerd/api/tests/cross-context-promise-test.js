import { match, rejects, strictEqual, throws } from 'assert';
import { AsyncLocalStorage } from 'async_hooks';
import { inspect } from 'util';
import { mock } from 'node:test';

export const crossContextResolveWorks = {
  async test(_, env) {
    // We're going to send two simultaneous requests to the same endpoint.
    const results = await Promise.allSettled([
      env.subrequest.fetch('http://example.org/resolve'),
      env.subrequest.fetch('http://example.org/resolve'),
    ]);
    strictEqual(results[0].status, 'fulfilled');
    strictEqual(results[1].status, 'fulfilled');
    strictEqual(results[0].value.status, 200);
    strictEqual(results[1].value.status, 200);
    strictEqual(await results[0].value.text(), 'ok');
    strictEqual(await results[1].value.text(), 'ok');
  },
};

export const crossContextRejectWorks = {
  async test(_, env) {
    // We're going to send two simultaneous requests to the same endpoint.
    const results = await Promise.allSettled([
      env.subrequest.fetch('http://example.org/reject'),
      env.subrequest.fetch('http://example.org/reject'),
    ]);
    strictEqual(results[0].status, 'fulfilled');
    strictEqual(results[1].status, 'fulfilled');
    strictEqual(results[0].value.status, 200);
    strictEqual(results[1].value.status, 200);
    strictEqual(await results[0].value.text(), 'ok');
    strictEqual(await results[1].value.text(), 'ok');
  },
};

export const crossContextStreamWorks = {
  async test(_, env) {
    // We're going to send two simultaneous requests to the same endpoint.
    const results = await Promise.allSettled([
      env.subrequest.fetch('http://example.org/stream'),
      env.subrequest.fetch('http://example.org/stream'),
    ]);
    strictEqual(results[0].status, 'fulfilled');
    strictEqual(results[1].status, 'fulfilled');
    strictEqual(results[0].value.status, 200);
    strictEqual(results[1].value.status, 200);
    strictEqual(await results[0].value.text(), 'ok');
    strictEqual(await results[1].value.text(), 'ok');
  },
};

export const customThenableWorks = {
  async test(_, env) {
    // We're going to send two simultaneous requests to the same endpoint.
    const results = await Promise.allSettled([
      env.subrequest.fetch('http://example.org/thenable'),
      env.subrequest.fetch('http://example.org/thenable'),
    ]);
    strictEqual(results[0].status, 'fulfilled');
    strictEqual(results[1].status, 'fulfilled');
    strictEqual(results[0].value.status, 200);
    strictEqual(results[1].value.status, 200);
    strictEqual(await results[0].value.text(), 'ok');
    strictEqual(await results[1].value.text(), 'ok');
  },
};

export const unhandledRejectionWorks = {
  async test(_, env) {
    // We're going to send two simultaneous requests to the same endpoint.
    const results = await Promise.allSettled([
      env.subrequest.fetch('http://example.org/unhandled'),
      env.subrequest.fetch('http://example.org/unhandled'),
    ]);
    strictEqual(results[0].status, 'fulfilled');
    strictEqual(results[1].status, 'fulfilled');
    strictEqual(results[0].value.status, 200);
    strictEqual(results[1].value.status, 200);
    strictEqual(await results[0].value.text(), 'ok');
    strictEqual(await results[1].value.text(), 'ok');
  },
};

export const expiredContextWorks = {
  async test(_, env) {
    // We're going to send two simultaneous requests to the same endpoint.
    const results = await Promise.allSettled([
      env.subrequest.fetch('http://example.org/expired'),
      env.subrequest.fetch('http://example.org/expired'),
    ]);
    strictEqual(results[0].status, 'rejected');
    strictEqual(results[1].status, 'fulfilled');
    strictEqual(
      results[0].reason.message,
      'The script will never generate a response.'
    );
    strictEqual(results[1].value.status, 200);
    strictEqual(await results[1].value.text(), 'ok');
    // Wait a tick for things to settle out before checking the global.
    // We're just making sure here that the promise in the first request
    // was canceled correctly.
    await scheduler.wait(100);
    strictEqual(globalThis.expiredRan, undefined);
  },
};

export const asyncIterWorks = {
  async test(_, env) {
    // We're going to send two simultaneous requests to the same endpoint.
    const results = await Promise.allSettled([
      env.subrequest.fetch('http://example.org/asynciter'),
      env.subrequest.fetch('http://example.org/asynciter'),
    ]);
    strictEqual(results[0].status, 'fulfilled');
    strictEqual(results[1].status, 'fulfilled');
    strictEqual(results[0].value.status, 200);
    strictEqual(results[1].value.status, 200);
    strictEqual(await results[0].value.text(), 'ok');
    strictEqual(await results[1].value.text(), 'ok');
  },
};

export const cyclicAwaitsWorks = {
  async test(_, env) {
    // We're going to send two simultaneous requests to the same endpoint.
    const results = await Promise.allSettled([
      env.subrequest.fetch('http://example.org/cyclic'),
      env.subrequest.fetch('http://example.org/cyclic'),
    ]);
    strictEqual(results[0].status, 'rejected');
    strictEqual(results[1].status, 'fulfilled');
  },
};

export default {
  async fetch(req, env, ctx) {
    if (req.url.endsWith('/resolve')) {
      return resolveTest(req, env, ctx);
    } else if (req.url.endsWith('/reject')) {
      return rejectTest(req, env, ctx);
    } else if (req.url.endsWith('/stream')) {
      return crossRequestStream(req, env, ctx);
    } else if (req.url.endsWith('/thenable')) {
      return customThenable(req, env, ctx);
    } else if (req.url.endsWith('/unhandled')) {
      return unhandledRejection(req, env, ctx);
    } else if (req.url.endsWith('/expired')) {
      return expiredContext(req, env, ctx);
    } else if (req.url.endsWith('/asynciter')) {
      return asyncIterator(req, env, ctx);
    } else if (req.url.endsWith('/cyclic')) {
      return cyclicPromise(req, env, ctx);
    }
    throw new Error('Invalid URL');
  },
};

function setupWaiter(ctx) {
  const { promise, resolve } = Promise.withResolvers();
  setTimeout(resolve, 1000);
  ctx.waitUntil(promise);
}

async function resolveTest(req, env, ctx) {
  const als = new AsyncLocalStorage();
  // This will be called twice. The first time in one request where we will
  // create the promise and resolver. The second time for the second request
  // where the promise will be resolved.
  if (globalThis.request1 === undefined) {
    setupWaiter(ctx);
    const { promise, resolve } = Promise.withResolvers();
    globalThis.request1 = { promise, resolve };
    const ab = AbortSignal.abort();
    strictEqual(ab.aborted, true);
    await als.run(123, async () => {
      await promise;
      strictEqual(als.getStore(), 123);
    });
    // This part is the main test. It will not run until after the promise
    // is resolved in the second request.
    // We use an AbortSignal because it is bound to the IoContext and will
    // throw an error if ab.aborted is checked from the wrong IoContext.
    // If this line runes, it is proof that the promise continuation is
    // running in the correct IoContext.
    strictEqual(ab.aborted, true);
    return new Response('ok');
  }

  // This is our second request. Here, all we do is resolve the promise.

  // While we are deferring the continuations from the promise, the promise state
  // change should happen immediately. Before calling resolve, the state should
  // be pending. After calling resolve, the state should be resolved showing
  // an undefined value. Updating the state of the promise immediately and
  // synchronously is required by the language specification.
  // See: https://tc39.es/ecma262/#sec-promise-resolve-functions
  strictEqual(inspect(globalThis.request1.promise), 'Promise { <pending> }');
  als.run('abc', () => globalThis.request1.resolve());
  strictEqual(inspect(globalThis.request1.promise), 'Promise { undefined }');

  const p = globalThis.request1.promise;

  globalThis.request1 = undefined;

  // We ought to be able to do a cross-request wait on the promise still.
  await p;

  return new Response('ok');
}

const reason = new Error('boom');

async function rejectTest(req, env, ctx) {
  if (globalThis.request2 === undefined) {
    setupWaiter(ctx);
    const { promise, reject } = Promise.withResolvers();
    globalThis.request2 = { reject };
    const ab = AbortSignal.abort();
    strictEqual(ab.aborted, true);
    try {
      // The promise will be rejected from the other request.
      await promise;
      throw new Error('should not get here');
    } catch (err) {
      // The reason provided by the other request should be carried
      // through here. If the ab.aborted check throws, then the continuation
      // is running in the wrong IoContext, which is the main thing we are
      // testing for here.
      strictEqual(err, reason);
      strictEqual(ab.aborted, true);
    }
    return new Response('ok');
  }

  // This is our second request. Here, all we do is reject the promise.
  globalThis.request2.reject(reason);
  globalThis.request2 = undefined;
  return new Response('ok');
}

async function crossRequestStream(req, env, ctx) {
  // Here, we are going to create a stream that will be used across
  // requests. The first request will create the stream and queue up
  // a pending read. The second request will fulfill the read by providing
  // data to the stream.
  if (globalThis.stream === undefined) {
    setupWaiter(ctx);
    let controller;
    const readable = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    globalThis.stream = { controller };
    const reader = readable.getReader();
    const ab = AbortSignal.abort();
    strictEqual(ab.aborted, true);
    const read = await reader.read();
    strictEqual(ab.aborted, true);
    return new Response('ok');
  }

  // This is our second request. Here, all we do is provide data to the stream.
  // This will cause our pending read to be fulfilled.
  const enc = new TextEncoder();
  globalThis.stream.controller.enqueue(enc.encode('hello'));
  globalThis.stream = undefined;
  return new Response('ok');
}

async function customThenable(req, env, ctx) {
  // This will be called twice. The first time in one request where we will
  // create the promise and resolver. The second time for the second request
  // where the promise will be resolved.
  if (globalThis.thenable === undefined) {
    setupWaiter(ctx);
    const { promise, resolve } = Promise.withResolvers();
    globalThis.thenable = { resolve };
    const ab = AbortSignal.abort();
    strictEqual(ab.aborted, true);

    // We check to make sure the value provided by the custom thenable is
    // property passed through to the promise resolution.

    strictEqual(await promise, 1);
    // This part is the main test. It will not run until after the promise
    // is resolved in the second request.
    // We use an AbortSignal because it is bound to the IoContext and will
    // throw an error if ab.aborted is checked from the wrong IoContext.
    // If this line runes, it is proof that the promise continuation is
    // running in the correct IoContext.
    strictEqual(ab.aborted, true);
    return new Response('ok');
  }

  // This is our second request. Here, all we do is resolve the promise.
  const ab = AbortSignal.abort();
  strictEqual(ab.aborted, true);

  const then = mock.fn((resolve) => {
    // The thenable should be invoked in the second request's IoContext.
    // If it is not, then the ab.aborted check below will fail.
    strictEqual(ab.aborted, true);
    resolve(1);
  });

  globalThis.thenable.resolve({ then });
  globalThis.thenable = undefined;

  // Confirm that the thenable was called exactly once in this request.
  await Promise.resolve();
  strictEqual(then.mock.calls.length, 1);

  return new Response('ok');
}

async function unhandledRejection(req, env, ctx) {
  // This will be called twice. The first time in one request where we will
  // create the promise and resolver. The second time for the second request
  // where the promise will be rejected.
  if (globalThis.unhandled === undefined) {
    setupWaiter(ctx);
    const { promise, reject } = Promise.withResolvers();
    globalThis.unhandled = { reject };
    const ab = AbortSignal.abort();
    strictEqual(ab.aborted, true);

    const rejectPromise = Promise.withResolvers();
    globalThis.addEventListener(
      'unhandledrejection',
      (event) => {
        // Here we have a gotcha! The unhandledrejection event is dispatched
        // synchronously when the promise is rejected. It does not get deferred.
        // so the IoContext here will be the second request's IoContext! This
        // means that our ab.aborted check will fail!
        throws(() => ab.aborted, {
          message: /I\/O type: RefcountedCanceler/,
        });
        strictEqual(event.reason, reason);
        rejectPromise.resolve();
      },
      { once: true }
    );
    await rejectPromise.promise;

    return new Response('ok');
  }

  // This is our second request. Here, all we do is reject the promise.
  globalThis.unhandled.reject(reason);
  globalThis.unhandled = undefined;
  return new Response('ok');
}

async function expiredContext(req, env, ctx) {
  if (globalThis.expired === undefined) {
    // We do not arrange for a waiter here. We want the request context
    // to be canceled before the promise is resolved. In the error log
    // (which we unfortunately cannot check here) we should see a message
    // about the hanging promise being canceled and another message about
    // a promise resolved across request contexts but the target request
    // is not longer active. On the calling side, the first request
    // should reject and the second request should resolve.
    const { promise, resolve } = Promise.withResolvers();
    globalThis.expired = { resolve };
    promise.then(() => {
      // This should never run...
      globalThis.expiredRan = true;
    });
    await new Promise(() => {});
    return new Response('ok');
  }

  // This is our second request. Here, all we do is resolve the promise.
  // Let's wait a bit to make sure the other request has had time to
  // be canceled and destroyed.
  await scheduler.wait(100);
  globalThis.expired.resolve();
  globalThis.expired = undefined;
  return new Response('ok');
}

async function* gen(ab) {
  let c = 0;
  for (;;) {
    await scheduler.wait(10);
    strictEqual(ab.aborted, true);
    yield c++;
  }
}

async function asyncIterator(req, env, ctx) {
  if (globalThis.asynciter === undefined) {
    const ab = AbortSignal.abort();
    globalThis.asynciter = gen(ab);
    globalThis.asyncIter2 = gen(ab);
    return new Response('ok');
  }

  // Advancing the iterator should throw an I/O error because the generator
  // is bound to the wrong IoContext here. This test just confirms that our
  // promise context patch does not change the expected behavior of the async
  // generator and async iterators. Those should still throw I/O errors when
  // the generator is bound to the wrong IoContext via something that it captures.
  await rejects(globalThis.asynciter.next(), {
    message: /Cannot perform I\/O/,
  });

  const foo = {
    [Symbol.asyncIterator]() {
      return globalThis.asyncIter2;
    },
  };

  try {
    for await (const _ of foo) {
    }
    throw new Error('should not get here');
  } catch (err) {
    match(err.message, /Cannot perform I\/O/);
  }

  return new Response('ok');
}

async function cyclicPromise(req, env, ctx) {
  if (globalThis.cyclic === undefined) {
    setupWaiter(ctx);
    const { promise, resolve } = Promise.withResolvers();
    globalThis.cyclic = { promise, resolve };
    const ab = AbortSignal.abort();
    strictEqual(ab.aborted, true);
    await promise;
    throw new Error('should never get here');
  }

  async function foo() {
    await globalThis.cyclic.promise;
    throw new Error('shnould never get here');
  }

  // The first request will hang because the promise is resolved
  // with a promise that waits on the first, creating a hang
  // condition that we can detect. The test log will contain
  // a message about a hanging promise being canceled.
  globalThis.cyclic.resolve(foo());
  globalThis.cyclic = undefined;

  return new Response('ok');
}
