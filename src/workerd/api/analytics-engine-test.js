import * as assert from 'node:assert';

let written = false;
async function isWritten(timeout) {
  const start = Date.now();
  do {
    if (written) return true;
    await scheduler.wait(100);
  } while (Date.now() - start < timeout);
  throw new Error('Test never received request from analytics engine handler');
}

export default {
  async fetch(ctrl, env, ctx) {
    written = true;
    return new Response('');
  },
  async test(ctrl, env, ctx) {
    env.aebinding.writeDataPoint({
      blobs: ['TestBlob'],
      doubles: [25],
      indexes: ['testindex'],
    });

    assert.equal(await isWritten(5000), true);
    return new Response('');
  },
};
