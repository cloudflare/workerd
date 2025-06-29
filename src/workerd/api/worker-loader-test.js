import { DurableObject } from 'cloudflare:workers';
import assert from 'node:assert';

export let basics = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('basics', () => {
      return {
        compatibilityDate: '2025-01-01',
        mainModule: 'foo.js',
        modules: {
          'foo.js': `
            export default {
              greet(name) { return "Hello, " + name; }
            }
            export let alternate = {
              greet(name) { return "Welcome, " + name; }
            }
          `,
        },
      };
    });

    {
      let result = await worker.getEntrypoint().greet('Alice');
      assert.strictEqual(result, 'Hello, Alice');
    }

    {
      let result = await worker.getEntrypoint('alternate').greet('Bob');
      assert.strictEqual(result, 'Welcome, Bob');
    }
  },
};

// Test supplying a basic `env` object.
export let passEnv = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('passEnv', () => {
      return {
        compatibilityDate: '2025-01-01',
        mainModule: 'foo.js',
        modules: {
          'foo.js': `
            export default {
              fetch(req, env, ctx) {
                return new Response("env.hello = " + env.hello);
              },
            }
          `,
        },
        env: {
          hello: 123,
        },
      };
    });

    let resp = await worker.getEntrypoint().fetch('https://example.com');
    assert.strictEqual(await resp.text(), 'env.hello = 123');
  },
};

export let testOutbound = {
  async fetch(req, env, ctx) {
    return new Response('hello from testOutbound');
  },
};

// Test overriding globalOutbound
export let overrideGlobalOutbound = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('overrideGlobalOutbound', () => {
      return {
        compatibilityDate: '2025-01-01',
        mainModule: 'foo.js',
        modules: {
          'foo.js': `
            export default {
              fetch(req, env, ctx) {
                return fetch(req);
              },
            }
          `,
        },

        // Set globalOutbound to redirect to our own `testOutbound` entrypoint.
        globalOutbound: ctx.exports.testOutbound,
      };
    });

    let resp = await worker.getEntrypoint().fetch('https://example.com');
    assert.strictEqual(await resp.text(), 'hello from testOutbound');
  },
};

export class FacetTestActor extends DurableObject {
  async doTest() {
    let worker = this.env.loader.get('facets', () => {
      return {
        compatibilityDate: '2025-01-01',
        mainModule: 'foo.js',
        modules: {
          'foo.js': `
            import {DurableObject} from "cloudflare:workers";
            export class MyActor extends DurableObject {
              i = 0;

              increment(j = 1) {
                this.i += j;
                return this.i;
              }
            }
          `,
        },

        // Set globalOutbound to redirect to our own `testOutbound` entrypoint.
        globalOutbound: this.ctx.exports.testOutbound,
      };
    });

    let cls = worker.getDurableObjectClass('MyActor');

    let facet = this.ctx.facets.get('bar', { class: cls });

    assert.strictEqual(await facet.increment(), 1);
    assert.strictEqual(await facet.increment(4), 5);
  }
}

export let facets = {
  async test(ctrl, env, ctx) {
    let id = ctx.exports.FacetTestActor.idFromName('foo');
    let stub = ctx.exports.FacetTestActor.get(id);
    await stub.doTest();
  },
};

// Test isolate uniqueness when loading the same name multiple times.
export let isolateUniqueness = {
  async test(ctrl, env, ctx) {
    let workerCode = {
      compatibilityDate: '2025-01-01',
      mainModule: 'foo.js',
      modules: {
        'foo.js': `
          import {WorkerEntrypoint} from "cloudflare:workers";
          let i = 0;
          export default class extends WorkerEntrypoint {
            increment() {
              return i++;
            }
          }
        `,
      },
    };
    let name = 'isolateUniqueness';
    let name2 = 'isolateUniquenessOtherName';

    // Load the same name from all four of our loaders:
    // - Two loaders with the same ID.
    // - One with a unique ID.
    // - One with no ID (anonymous).
    let shared1 = env.sharedLoader1.get(name, () => workerCode).getEntrypoint();
    let shared2 = env.sharedLoader2.get(name, () => workerCode).getEntrypoint();
    let unique = env.uniqueLoader.get(name, () => workerCode).getEntrypoint();
    let anonymous = env.loader.get(name, () => workerCode).getEntrypoint();

    // Also try loading a different name from various loaders.
    let anonymousOtherName = env.loader
      .get(name2, () => workerCode)
      .getEntrypoint();
    let sharedOtherName = env.sharedLoader1
      .get(name2, () => workerCode)
      .getEntrypoint();

    // Same name from loaders with the same ID should go to the same isolate.
    assert.strictEqual(await shared1.increment(), 0);
    assert.strictEqual(await shared1.increment(), 1);
    assert.strictEqual(await shared2.increment(), 2);
    assert.strictEqual(await shared2.increment(), 3);

    // Different IDs -> different isolates.
    assert.strictEqual(await anonymous.increment(), 0);
    assert.strictEqual(await anonymous.increment(), 1);
    assert.strictEqual(await unique.increment(), 0);
    assert.strictEqual(await unique.increment(), 1);

    // Different names -> different isolates.
    assert.strictEqual(await anonymousOtherName.increment(), 0);
    assert.strictEqual(await anonymousOtherName.increment(), 1);
    assert.strictEqual(await sharedOtherName.increment(), 0);
    assert.strictEqual(await sharedOtherName.increment(), 1);

    // Load the same name from the same loader -> same isolate.
    let anonymousAgain = env.loader.get(name, () => workerCode).getEntrypoint();
    assert.strictEqual(await anonymousAgain.increment(), 2);
    assert.strictEqual(await anonymousAgain.increment(), 3);
  },
};
