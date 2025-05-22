import { rejects } from 'node:assert';

export default {
  async test(ctrl, env) {
    await rejects(env.pythonWorker.fetch(new Request('http://example.com')), {
      name: 'Error',
      message: 'Handler does not export a fetch() function.',
    });
  },
};
