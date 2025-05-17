import assert from 'node:assert';

export let testHangingPromise = {
  async test(controller, env, ctx) {
    await assert.rejects(
      ctx.exports.testHangingPromise.fetch('http://example.com'),
      {
        name: 'Error',
        message: 'The script will never generate a response.',
      }
    );
  },

  async fetch(req, env, ctx) {
    await new Promise((resolve) => {});
  },
};
