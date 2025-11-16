import assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

export class TestActor extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    ctx.addEventListener('unload', () => {
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
    });
  }
  async fetch() {
    return new Response('actor ok');
  }
}

export const unloadEventTest = {
  async test(req, env, ctx) {
    ctx.addEventListener('unload', () => {
      queueMicrotask(() => {
        console.log('NEVER');
      });
      console.log('UNLOAD TEST UNLOAD FIRED');
      let p = new Promise((resolve) => setTimeout(resolve));
      p.then(() => {
        throw new Error('unexpected io');
      });
      throw new Error('handler error');
    });
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
    ctx.addEventListener('unload', () => {
      console.log('ACTOR TEST UNLOAD FIRED');
    });
    const id = env.TEST_ACTOR.idFromName('test');
    const actor = env.TEST_ACTOR.get(id);
    const resp = await actor.fetch(new Request('https://placeholder/test'));
    const body = await resp.text();
    console.log(body);
    await new Promise((resolve) => setTimeout(resolve, 500));
  },
};

let pastCtx;
export class StorageUnloadActor extends DurableObject {
  storageHandler = () => {
    console.log('STORAGE UNLOAD HANDLER FIRED');
    this.ctx.storage.put('unload-key', 'unload-value').catch((e) => {
      console.log('NEVER'); // (microtasks cleared)
    });
  };

  replacementHandler = () => {
    console.log('REPLACEMENT HANDLER FIRED');
  };

  constructor(ctx, env) {
    super(ctx, env);

    if (pastCtx) {
      console.log('past ctx is different instance');
      assert.notStrictEqual(ctx, pastCtx);
    } else {
      pastCtx = ctx;
    }

    ctx.addEventListener('unload', this.storageHandler);
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
    if (url.pathname === '/remove-listener') {
      this.ctx.removeEventListener('unload', this.storageHandler);
      this.ctx.addEventListener('unload', this.replacementHandler);
      return new Response('listener removed and replaced');
    }
    return new Response('storage actor ok');
  }
}

export const storageUnloadTest = {
  async test(req, env, ctx) {
    const id = env.STORAGE_UNLOAD_ACTOR.idFromName('storage-test');
    const actor = env.STORAGE_UNLOAD_ACTOR.get(id);

    const resp1 = await actor.fetch(new Request('https://placeholder/test'));
    const body1 = await resp1.text();
    assert.strictEqual(body1, 'storage actor ok');
    console.log('Storage test: first request complete');

    await new Promise((resolve) => setTimeout(resolve, 100));

    const resp2 = await actor.fetch(
      new Request('https://placeholder/remove-listener')
    );
    const body2 = await resp2.text();
    assert.strictEqual(body2, 'listener removed and replaced');
    console.log('Storage test: listener removed and replaced');

    await new Promise((resolve) => setTimeout(resolve, 100));

    const resp3 = await actor.fetch(new Request('https://placeholder/check'));
    const body3 = await resp3.text();
    assert.strictEqual(body3, 'write was blocked');

    // Wait for actor to be evicted and fire its unload handler before exiting the test
    await new Promise((resolve) => setTimeout(resolve, 150));
  },
};

let pastWorkerCtx;
export default {
  async fetch(req, env, ctx) {
    console.log('Fetch');

    if (pastWorkerCtx) {
      assert.notStrictEqual(ctx, pastWorkerCtx);
      console.log('past worker ctx is different instance');
    } else {
      pastWorkerCtx = ctx;
    }

    ctx.addEventListener('unload', () => {
      queueMicrotask(() => {
        console.log('NEVER');
      });
      console.log('FETCH UNLOAD FIRED');
    });
    return new Response('ok');
  },
};

export const workerOnUnloadTest = {
  async test(req, env, ctx) {
    const resp1 = await env.SERVICE.fetch(
      new Request('https://placeholder/test1')
    );
    const body1 = await resp1.text();
    assert.strictEqual(body1, 'ok');
    console.log('Worker test: first request complete');

    const resp2 = await env.SERVICE.fetch(
      new Request('https://placeholder/test2')
    );
    const body2 = await resp2.text();
    assert.strictEqual(body2, 'ok');
    console.log('Worker test: verified each request gets new ctx');
  },
};
