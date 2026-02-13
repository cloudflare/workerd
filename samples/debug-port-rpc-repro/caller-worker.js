export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const test = url.searchParams.get('test');
    try {
      if (test === 'direct-rpc') {
        return new Response('direct-rpc: ' + (await env.DIRECT.ping()));
      }
      if (test === 'proxy-rpc') {
        return new Response('proxy-rpc: ' + (await env.PROXY.ping()));
      }
      if (test === 'props-rpc') {
        return new Response('props-rpc: ' + (await env.PROPS.ping()));
      }
      if (test === 'direct-fetch') {
        return env.DIRECT.fetch(request);
      }
      if (test === 'proxy-fetch') {
        return env.PROXY.fetch(request);
      }
      if (test === 'props-fetch') {
        return env.PROPS.fetch(request);
      }
      return new Response(
        'Use ?test=direct-rpc|proxy-rpc|props-rpc|direct-fetch|proxy-fetch|props-fetch'
      );
    } catch (e) {
      return new Response('ERROR: ' + e.message, { status: 500 });
    }
  },
};
