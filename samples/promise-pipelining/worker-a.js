export default {
  async fetch(request, env) {
    const foo = env.PROXY.foo('🐜');
    const buzzResult = await foo.bar.buzz();
    return new Response(buzzResult);
  },
};
