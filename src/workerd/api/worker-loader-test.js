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
