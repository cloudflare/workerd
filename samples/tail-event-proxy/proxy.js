export default {
  async tail(events, env) {
    await env.TAIL_WORKER.tail(events);
  },
};
