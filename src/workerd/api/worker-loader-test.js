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

export let pythonBasics = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('pythonBasics', () => {
      return {
        compatibilityDate: '2025-01-01',
        mainModule: 'foo.py',
        compatibilityFlags: ['python_workers', 'python_no_global_handlers'],
        modules: {
          'foo.py': `
from workers import WorkerEntrypoint
class Default(WorkerEntrypoint):
  async def greet(self, name):
    return "Hello, " + name
class alternate(WorkerEntrypoint):
  async def greet(self, name):
    return "Welcome, " + name
          `,
        },
      };
    });

    {
      let result = await worker.getEntrypoint().greet('Stacey');
      assert.strictEqual(result, 'Hello, Stacey');
    }

    {
      let result = await worker.getEntrypoint('alternate').greet('Anthony');
      assert.strictEqual(result, 'Welcome, Anthony');
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

// This test's own `globalOutbound` is configured to loop back to this entrypoint.
export let defaultOutbound = {
  async fetch(req, env, ctx) {
    return new Response('hello from defaultOutbound');
  },
};

// Test that globalOutbound is inherited if not specified.
export let inheritGlobalOutbound = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('inheritGlobalOutbound', () => {
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

        // Don't set `globalOutbound` at all.
      };
    });

    let resp = await worker.getEntrypoint().fetch('https://example.com');
    assert.strictEqual(await resp.text(), 'hello from defaultOutbound');
  },
};

// Test setting `globalOutbound` to null.
export let nullGlobalOutbound = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('nullGlobalOutbound', () => {
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

        globalOutbound: null,
      };
    });

    await assert.rejects(worker.getEntrypoint().fetch('https://example.com'), {
      name: 'Error',
      message:
        'This worker is not permitted to access the internet via global functions like fetch(). ' +
        "It must use capabilities (such as bindings in 'env') to talk to the outside world.",
    });
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

    let facet = this.ctx.facets.get('bar', () => {
      return { class: cls };
    });

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
    let loadCount = 0;
    let loadCodeCallback = () => {
      ++loadCount;
      return {
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
    };

    let assertNotCalled = () => {
      throw new Error(
        'Code loader callback called when it should have reused an existing isolate.'
      );
    };

    let name = 'isolateUniqueness';
    let name2 = 'isolateUniquenessOtherName';

    // Load the same name from all four of our loaders:
    // - Two loaders with the same ID.
    // - One with a unique ID.
    // - One with no ID (anonymous).
    let shared1 = env.sharedLoader1.get(name, loadCodeCallback).getEntrypoint();
    let shared2 = env.sharedLoader2.get(name, assertNotCalled).getEntrypoint();
    let unique = env.uniqueLoader.get(name, loadCodeCallback).getEntrypoint();
    let anonymous = env.loader.get(name, loadCodeCallback).getEntrypoint();

    // Also try loading a different name from various loaders.
    let anonymousOtherName = env.loader
      .get(name2, loadCodeCallback)
      .getEntrypoint();
    let sharedOtherName = env.sharedLoader1
      .get(name2, loadCodeCallback)
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
    let anonymousAgain = env.loader.get(name, assertNotCalled).getEntrypoint();
    assert.strictEqual(await anonymousAgain.increment(), 2);
    assert.strictEqual(await anonymousAgain.increment(), 3);

    assert.strictEqual(loadCount, 5);
  },
};

// Test non-string module types
export let moduleTypes = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('moduleTypes', () => {
      return {
        compatibilityDate: '2025-01-01',
        mainModule: 'main.js',
        modules: {
          'main.js': {
            js: `
              import {WorkerEntrypoint} from "cloudflare:workers";
              import textModule from './text.txt';
              import jsonModule from './data.json';
              import cjsModule from './cjs.cjs';
              import dataModule from './binary.dat';

              export default class extends WorkerEntrypoint {
                getTextModule() { return textModule; }
                getJsonModule() { return jsonModule; }
                getCjsModule() { return cjsModule.greet('Alice'); }
                getDataModule() { return dataModule; }
              }
            `,
          },
          'text.txt': {
            text: 'Hello from text module!',
          },
          'data.json': {
            json: { message: 'Hello from JSON module!', value: 42 },
          },
          'cjs.cjs': {
            cjs: `
              module.exports = {
                greet: function(name) {
                  return 'Hello from CommonJS, ' + name;
                }
              };
            `,
          },
          'binary.dat': {
            data: new TextEncoder().encode('Hello from binary data!'),
          },
        },
      };
    });

    let entrypoint = worker.getEntrypoint();

    assert.strictEqual(
      await entrypoint.getTextModule(),
      'Hello from text module!'
    );

    let jsonData = await entrypoint.getJsonModule();
    assert.strictEqual(jsonData.message, 'Hello from JSON module!');
    assert.strictEqual(jsonData.value, 42);

    assert.strictEqual(
      await entrypoint.getCjsModule(),
      'Hello from CommonJS, Alice'
    );

    let dataModule = await entrypoint.getDataModule();
    let decoder = new TextDecoder();
    assert.strictEqual(decoder.decode(dataModule), 'Hello from binary data!');
  },
};

// Test setting compat date / flags works
export let compatDateFlags = {
  async test(ctrl, env, ctx) {
    let getCode = (compatibilityDate, compatibilityFlags) => {
      return {
        compatibilityDate,
        compatibilityFlags,
        allowExperimental: true,
        mainModule: 'main.js',
        modules: {
          'main.js': `
            import {WorkerEntrypoint} from "cloudflare:workers";
            export default class extends WorkerEntrypoint {
              hasNodeCompat() {
                return typeof process !== 'undefined';
              }
              canUseRequestCache() {
                try {
                  new Request("https://foo", {cache: "no-store"});
                  return true;
                } catch {
                  return false;
                }
              }
            }
          `,
        },
      };
    };

    // Test old date (2023), with node compat turned on.
    {
      let worker = env.loader.get('compatDateFlags1', () =>
        getCode('2023-01-01', ['nodejs_compat', 'nodejs_compat_v2'])
      );
      let entrypoint = await worker.getEntrypoint();
      assert.strictEqual(await entrypoint.hasNodeCompat(), true);
      assert.strictEqual(await entrypoint.canUseRequestCache(), false);
    }

    // Test newer date (2025), with node compat not enabled.
    {
      let worker = env.loader.get('compatDateFlags2', () =>
        getCode('2025-01-01', [])
      );
      let entrypoint = await worker.getEntrypoint();
      assert.strictEqual(await entrypoint.hasNodeCompat(), false);
      assert.strictEqual(await entrypoint.canUseRequestCache(), true);
    }
  },
};

// Test code loader callback that does something asynchronous
export let asyncCodeLoader = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('asyncCodeLoader', async () => {
      // Simulate async work
      await new Promise((resolve) => setTimeout(resolve, 1));

      return {
        compatibilityDate: '2025-01-01',
        mainModule: 'main.js',
        modules: {
          'main.js': `
            import {WorkerEntrypoint} from "cloudflare:workers";
            export default class extends WorkerEntrypoint {
              getMessage() {
                return 'Hello from async loaded worker!';
              }
            }
          `,
        },
      };
    });

    let result = await worker.getEntrypoint().getMessage();
    assert.strictEqual(result, 'Hello from async loaded worker!');
  },
};

// Test what happens if the code getter callback throws an exception
export let codeLoaderException = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('codeLoaderException', () => {
      throw new Error('Code loader failed!');
    });

    let ep = worker.getEntrypoint();
    try {
      await ep.someMethod();
      assert.fail('Expected exception to be thrown');
    } catch (error) {
      assert.strictEqual(error.message, 'Code loader failed!');
    }
  },
};

// Test that ctx.exports works in dynamically-loaded workers.
export let ctxExports = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('ctxExports', () => {
      return {
        compatibilityDate: '2025-01-01',
        compatibilityFlags: ['experimental'],
        mainModule: 'foo.js',
        allowExperimental: true,
        modules: {
          'foo.js': `
            import {WorkerEntrypoint} from "cloudflare:workers";
            export default class extends WorkerEntrypoint {
              async greet(name) {
                let greeting = await this.ctx.exports.AltEntrypoint.greet(name);
                return greeting + "!";
              }
            }
            export class AltEntrypoint extends WorkerEntrypoint {
              greet(name) { return "Hello, " + name; }
            }
          `,
        },
      };
    });

    let result = await worker.getEntrypoint().greet('Alice');
    assert.strictEqual(result, 'Hello, Alice!');
  },
};

const mixedModules = {
  compatibilityDate: '2025-01-01',
  modules: {
    'foo.py': `
from workers import WorkerEntrypoint
class Default(WorkerEntrypoint):
async def greet(self, name):
return "Hello, " + name
    `,
    'foo.js': `
      export default {
        greet(name) { return "Hello, " + name; }
      }
    `,
  },
};

export let noMixedJsPythonModules = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('noMixedJsPythonModules', () => {
      return {
        ...mixedModules,
        mainModule: 'foo.py',
      };
    });

    await assert.rejects(worker.getEntrypoint().greet('Alice'), {
      name: 'TypeError',
      message:
        'Module "foo.js" is a JS module, but the main module is a Python module.',
    });
  },
};

export let noMixedJsPythonModules2 = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('noMixedJsPythonModules2', () => {
      return {
        ...mixedModules,
        mainModule: 'foo.js',
      };
    });

    await assert.rejects(worker.getEntrypoint().greet('Alice'), {
      name: 'TypeError',
      message:
        'Module "foo.py" is a Python module, but the main module isn\'t a Python module.',
    });
  },
};
