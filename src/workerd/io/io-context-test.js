import assert from 'node:assert';

export let testHangingPromise = {
  async test(controller, env, ctx) {
    await assert.rejects(
      ctx.exports.testHangingPromise.fetch('http://example.com'),
      {
        name: 'Error',
        message:
          "The Workers runtime canceled this request because it detected that your Worker's " +
          'code had hung and would never generate a response. Refer to: ' +
          'https://developers.cloudflare.com/workers/observability/errors/',
      }
    );
  },

  async fetch(req, env, ctx) {
    await new Promise((resolve) => {});
  },
};

let abortCalled = false;
let abortReturned = false;

export let testAbort = {
  async test(controller, env, ctx) {
    await assert.rejects(ctx.exports.testAbort.fetch('http://example.com'), {
      name: 'Error',
      message: 'test abort reason',
    });

    assert.strictEqual(abortCalled, true);
    assert.strictEqual(abortReturned, false);
  },

  async fetch(req, env, ctx) {
    abortCalled = true;
    ctx.abort(new Error('test abort reason'));
    abortReturned = true; // shouldn't get here!
  },
};
