import http from 'node:http';
import { strictEqual } from 'node:assert';
import { httpServerHandler } from 'cloudflare:node';

const server = http.createServer((req, res) => {
  res.end('hello world');
});

// The server can be started by passing it to the httpServerHandler
// which will call the server's listen method if it hasn't already
// been called.
export default httpServerHandler(server);
strictEqual(typeof server.address().port, 'number');

export const checkItWorks = {
  async test(_ctrl, env) {
    const resp = await env.SUBREQUEST.fetch('http://example.com');
    strictEqual(await resp.text(), 'hello world');
  },
};
