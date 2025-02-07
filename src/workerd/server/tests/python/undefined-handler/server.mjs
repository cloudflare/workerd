import { rejects } from 'node:assert';

export default {
  async test(ctrl, env) {
    await rejects(env.pythonWorker.fetch(new Request('http://example.com')), {
      name: 'Error',
      message:
        'Python entrypoint "undefined_handler.py" does not export a handler named "on_fetch"',
    });
  },
};
