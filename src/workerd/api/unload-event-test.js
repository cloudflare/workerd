import assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

export class TestActor extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    ctx.addEventListener('unload', () => {
      console.log('ACTOR UNLOAD FIRED');
    });
    ctx.addEventListener('unload', () => {
      let p = new Promise((resolve) => setTimeout(resolve));
      p.then(() => {
        throw new Error('unexpected io');
      });
      throw new Error('actor unload error');
    });
    ctx.addEventListener('unload', () => {
      console.log('ACTOR UNLOAD 2 FIRED');
    });
  }
  async fetch() {
    return new Response('actor ok');
  }
}

export const unloadEventTest = {
  async test(req, env, ctx) {
    // Call fetch to register the unload listener
    queueMicrotask(() => {
      ctx.addEventListener('unload', () => {
        let p = new Promise((resolve) => setTimeout(resolve));
        p.then(() => {
          throw new Error('unexpected io');
        });
        throw new Error('handler error');
      });
      ctx.addEventListener('unload', () => {
        console.log('UNLOAD TEST UNLOAD FIRED 2');
      });
    });
    ctx.addEventListener('unload', () => {
      console.log('UNLOAD TEST UNLOAD FIRED');

      queueMicrotask(() => {
        ctx.addEventListener('unload', () => {
          console.log('NEVER');
        });
      });
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

export default {
  async fetch(req, env, ctx) {
    console.log('Fetch');
    ctx.addEventListener('unload', () => {
      console.log('FETCH UNLOAD FIRED');
    });
    return new Response('ok');
  },
};
