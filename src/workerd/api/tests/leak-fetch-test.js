export const memoryLeak = {
  async test(_ctrl, env) {
    const response = await env.subrequest.fetch('http://example.org');
    response.body?.pipeTo(new WritableStream());
  },
};

export default {
  async fetch() {
    await scheduler.wait(1_000);
    return new Response('ok');
  },
};
