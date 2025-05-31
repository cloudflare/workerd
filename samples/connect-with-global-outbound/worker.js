import { connect } from 'cloudflare:sockets';

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname == '/http') {
      return fetch('http://example.com');
    } else if (url.pathname == '/connect') {
      await connect('example.com:5432');
      return new Response('connection ok');
    }
    return new Response('try /http or /connect');
  },
};
