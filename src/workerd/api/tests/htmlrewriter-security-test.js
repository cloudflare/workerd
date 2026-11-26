// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects, strictEqual, throws } from 'node:assert';
import { AsyncLocalStorage } from 'node:async_hooks';

const complexityError = {
  name: 'TypeError',
  message: /selector exceeds maximum complexity of 256/,
};

function createStream(content) {
  const bytes = new TextEncoder().encode(content);
  return new ReadableStream({
    start(controller) {
      controller.enqueue(bytes);
      controller.close();
    },
  });
}

async function forceGc() {
  for (let i = 0; i < 10; i++) {
    gc();
    await scheduler.wait(0);
  }
}

async function withDeadline(promise, message) {
  return await Promise.race([
    promise,
    scheduler.wait(5_000).then(() => {
      throw new Error(message);
    }),
  ]);
}

function createPendingInput(content) {
  const started = Promise.withResolvers();
  const release = Promise.withResolvers();
  let sent = false;
  const stream = new ReadableStream({
    async pull(controller) {
      if (sent) return;
      sent = true;
      started.resolve();
      await release.promise;
      controller.enqueue(new TextEncoder().encode(content));
      controller.close();
    },
  });
  return { release, started: started.promise, stream };
}

export const reportedSelectorRecursionIsRejected = {
  test() {
    throws(
      () => new HTMLRewriter().on(Array(10_000).fill('div').join('>'), {}),
      complexityError
    );
    throws(
      () => new HTMLRewriter().on(Array(50_000).fill('*').join(' '), {}),
      complexityError
    );
    for (const fn of [':not(', ':is(', ':where(', ':has(']) {
      const selector = fn.repeat(257) + '*' + ')'.repeat(257);
      throws(() => new HTMLRewriter().on(selector, {}), complexityError);
    }
  },
};

export const selectorComplexityBoundaryIsAccepted = {
  async test() {
    const child = Array(257).fill('*').join('>');
    const descendant = Array(257).fill('*').join(' /**/ ');
    // lol-html's CSS parser rejects more than 75 nested blocks on its own, so
    // the 256-level nesting boundary is only reachable in
    // html-rewriter-selector-test.c++.
    const nested = ':not('.repeat(75) + '*' + ')'.repeat(75);
    const response = new HTMLRewriter()
      .on(child, {})
      .on(descendant, {})
      .on(nested, {})
      .transform(new Response('<p>x</p>'));
    strictEqual(await response.text(), '<p>x</p>');
  },
};

export const callbacksSurviveBuilderCollection = {
  async test() {
    const input = createPendingInput('<p>before</p>');
    let calls = 0;
    let rewriter = new HTMLRewriter().on('p', {
      element(element) {
        calls++;
        element.setInnerContent('after');
      },
    });
    let response = rewriter.transform(new Response(input.stream));
    const body = response.body;
    response = null;
    rewriter = null;

    await input.started;
    await forceGc();
    input.release.resolve();
    strictEqual(await new Response(body).text(), '<p>after</p>');
    strictEqual(calls, 1);
  },
};

export const dynamicEndTagCallbackSurvivesGc = {
  async test() {
    const firstChunkRead = Promise.withResolvers();
    const releaseEndTag = Promise.withResolvers();
    const endTagRegistered = Promise.withResolvers();
    const encoder = new TextEncoder();
    let pull = 0;
    const input = new ReadableStream({
      async pull(controller) {
        if (pull++ === 0) {
          controller.enqueue(encoder.encode('<p>before'));
          firstChunkRead.resolve();
        } else {
          await releaseEndTag.promise;
          controller.enqueue(encoder.encode('</p>'));
          controller.close();
        }
      },
    });

    let endCalls = 0;
    let rewriter = new HTMLRewriter().on('p', {
      element(element) {
        element.onEndTag(() => endCalls++);
        endTagRegistered.resolve();
      },
    });
    let response = rewriter.transform(new Response(input));
    const text = response.text();
    response = null;
    rewriter = null;

    await firstChunkRead.promise;
    await endTagRegistered.promise;
    await forceGc();
    releaseEndTag.resolve();
    strictEqual(await text, '<p>before</p>');
    strictEqual(endCalls, 1);
  },
};

export const replacementStreamSurvivesGc = {
  async test() {
    const replacementRegistered = Promise.withResolvers();
    const replacementStarted = Promise.withResolvers();
    const releaseReplacement = Promise.withResolvers();
    let pulled = 0;
    let replacement = new ReadableStream({
      async pull(controller) {
        replacementStarted.resolve();
        await releaseReplacement.promise;
        pulled++;
        controller.enqueue(new TextEncoder().encode('after'));
        controller.close();
      },
    });
    let rewriter = new HTMLRewriter().on('p', {
      element(element) {
        element.setInnerContent(replacement);
        replacementRegistered.resolve();
      },
    });
    const text = rewriter.transform(new Response('<p>before</p>')).text();

    await Promise.all([
      replacementRegistered.promise,
      replacementStarted.promise,
    ]);
    replacement = null;
    rewriter = null;
    await forceGc();
    releaseReplacement.resolve();
    strictEqual(await text, '<p>after</p>');
    strictEqual(pulled, 1);
  },
};

export const pendingHandlerAndAsyncContextSurviveGc = {
  async test() {
    const storage = new AsyncLocalStorage();
    const input = createPendingInput('<p>before</p>');
    const handlerStarted = Promise.withResolvers();
    const releaseHandler = Promise.withResolvers();
    let observedContext;
    let rewriter = new HTMLRewriter().on('p', {
      async element(element) {
        observedContext = storage.getStore();
        handlerStarted.resolve();
        await releaseHandler.promise;
        element.setInnerContent('after');
      },
    });
    let response = storage.run('transform-context', () =>
      rewriter.transform(new Response(input.stream))
    );
    const text = response.text();
    response = null;
    rewriter = null;

    await input.started;
    input.release.resolve();
    await handlerStarted.promise;
    await forceGc();
    releaseHandler.resolve();
    strictEqual(await text, '<p>after</p>');
    strictEqual(observedContext, 'transform-context');
  },
};

export const documentCallbackSurvivesGc = {
  async test() {
    const input = createPendingInput('<p>before</p>');
    let calls = 0;
    let rewriter = new HTMLRewriter().onDocument({
      end(document) {
        calls++;
        document.append('<p>after</p>', { html: true });
      },
    });
    let response = rewriter.transform(new Response(input.stream));
    const body = response.body;
    response = null;
    rewriter = null;

    await input.started;
    await forceGc();
    input.release.resolve();
    strictEqual(await new Response(body).text(), '<p>before</p><p>after</p>');
    strictEqual(calls, 1);
  },
};

export const abandonedReplacersAreDroppedOnFailure = {
  async test() {
    const before = createStream('before');
    const after = createStream('after');
    const replacement = createStream('replacement');
    let callbackCount = 0;
    const response = new HTMLRewriter()
      .on('p', {
        element(element) {
          callbackCount++;
          element.before(before);
          element.after(after);
          element.replace(replacement);
        },
      })
      .transform(new Response('<p>one</p><p>two</p><p>three</p>'));

    await rejects(response.text(), {
      name: 'TypeError',
      message: 'The ReadableStream has been locked to a reader.',
    });
    strictEqual(callbackCount, 2);
  },
};

export const pendingReplacerIsDroppedOnCancel = {
  async test() {
    const replacementStarted = Promise.withResolvers();
    const replacement = new ReadableStream({
      pull() {
        replacementStarted.resolve();
        return new Promise(() => {});
      },
    });
    let response = new HTMLRewriter()
      .on('p', {
        element(element) {
          element.replace(replacement);
        },
      })
      .transform(new Response('<p>before</p>'));
    let reader = response.body.getReader();
    const read = reader.read().then(
      () => {},
      () => {}
    );

    await withDeadline(replacementStarted.promise, 'replacement did not start');
    await withDeadline(
      reader.cancel(),
      'transformed body cancellation did not settle'
    );
    await withDeadline(read, 'pending transformed body read did not settle');
    response = null;
    reader = null;
    await forceGc();
  },
};

export const replacersAreDroppedWhenHandlerThrows = {
  async test() {
    const replacement = createStream('unused');
    const response = new HTMLRewriter()
      .on('p', {
        element(element) {
          element.replace(replacement);
          throw new Error('handler failed');
        },
      })
      .transform(new Response('<p>before</p>'));
    await rejects(response.text(), /handler failed/);
  },
};
