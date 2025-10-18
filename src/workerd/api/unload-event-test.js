import assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

export class TestActor extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    ctx.onunload = () => {
      console.log('ACTOR UNLOAD FIRED');
      let p1 = new Promise((resolve) => setTimeout(resolve));
      let p2 = fetch('https://placeholder/test');
      p1.then(() => {
        throw new Error('unexpected io');
      });
      p2.finally(() => {
        throw new Error('unexpected io');
      });
      throw new Error('actor unload error');
    };
  }
  async fetch() {
    return new Response('actor ok');
  }
}

export const unloadEventTest = {
  async test(req, env, ctx) {
    ctx.onunload = () => {
      queueMicrotask(() => {
        console.log('NEVER');
      });
      console.log('UNLOAD TEST UNLOAD FIRED');
      let p = new Promise((resolve) => setTimeout(resolve));
      p.then(() => {
        throw new Error('unexpected io');
      });
      throw new Error('handler error');
    };
    console.log('Fetching');
    const resp = await env.SERVICE.fetch(
      new Request('https://placeholder/test')
    );
    console.log('Fetched');
    const body = await resp.text();
    console.log('Test end');
    assert.strictEqual(body, 'ok');
  },
};

export const actorUnloadEventTest = {
  async test(req, env, ctx) {
    ctx.onunload = () => {
      console.log('ACTOR TEST UNLOAD FIRED');
    };
    const id = env.TEST_ACTOR.idFromName('test');
    const actor = env.TEST_ACTOR.get(id);
    const resp = await actor.fetch(new Request('https://placeholder/test'));
    const body = await resp.text();
    console.log(body);
    await new Promise((resolve) => setTimeout(resolve, 500));
  },
};

// Test that storage writes in unload handlers are blocked
let pastCtx;
let storedOnUnload;
let storageUnloadCount = 0;
export class StorageUnloadActor extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);

    // second run ctx check
    if (pastCtx) {
      console.log('past ctx onunload check');
      assert.strictEqual(ctx.onunload, undefined);
    } else {
      pastCtx = ctx;
    }

    ctx.onunload = () => {
      // Only log the first unload to avoid flaky test output from timing differences
      if (storageUnloadCount === 0) {
        console.log('STORAGE UNLOAD HANDLER FIRED');
        storageUnloadCount++;
      }
      ctx.storage.put('unload-key', 'unload-value').catch((e) => {
        console.log('NEVER'); // (microtasks cleared)
      });
    };
    storedOnUnload = ctx.onunload;
  }
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/check') {
      const value = await this.ctx.storage.get('unload-key');
      if (value === undefined) {
        return new Response('write was blocked');
      } else {
        return new Response('write persisted: ' + value);
      }
    }
    if (url.pathname === '/same-ref') {
      assert.strictEqual(this.ctx.onunload, storedOnUnload);
      return new Response('same reference');
    }
    return new Response('storage actor ok');
  }
}

export const storageUnloadTest = {
  async test(req, env, ctx) {
    const id = env.STORAGE_UNLOAD_ACTOR.idFromName('storage-test');
    const actor = env.STORAGE_UNLOAD_ACTOR.get(id);

    // First request
    const resp1 = await actor.fetch(new Request('https://placeholder/test'));
    const body1 = await resp1.text();
    assert.strictEqual(body1, 'storage actor ok');
    console.log('Storage test: first request complete');

    // Second request to same actor instance - verify same onunload reference
    const resp2 = await actor.fetch(
      new Request('https://placeholder/same-ref')
    );
    const body2 = await resp2.text();
    assert.strictEqual(body2, 'same reference');

    // Wait for actor to unload
    await new Promise((resolve) => setTimeout(resolve, 500));

    // Third request to check unload write did not persist
    const resp3 = await actor.fetch(new Request('https://placeholder/check'));
    const body3 = await resp3.text();
    assert.strictEqual(body3, 'write was blocked');
  },
};

let pastWorkerCtx;
export default {
  async fetch(req, env, ctx) {
    console.log('Fetch');

    // Second request - verify onunload from previous request is not accessible
    if (pastWorkerCtx) {
      console.log('past worker ctx onunload check');
      assert.strictEqual(pastWorkerCtx.onunload, undefined);
    } else {
      pastWorkerCtx = ctx;
    }

    ctx.onunload = () => {
      queueMicrotask(() => {
        console.log('NEVER');
      });
      console.log('FETCH UNLOAD FIRED');
    };
    return new Response('ok');
  },
};

export const workerOnUnloadTest = {
  async test(req, env, ctx) {
    // First request
    const resp1 = await env.SERVICE.fetch(
      new Request('https://placeholder/test1')
    );
    const body1 = await resp1.text();
    assert.strictEqual(body1, 'ok');
    console.log('Worker test: first request complete');

    // Second request - should not see onunload from first request
    const resp2 = await env.SERVICE.fetch(
      new Request('https://placeholder/test2')
    );
    const body2 = await resp2.text();
    assert.strictEqual(body2, 'ok');
    console.log('Worker test: verified onunload not persisted');
  },
};
