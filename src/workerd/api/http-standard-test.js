import { strictEqual } from 'node:assert';

export default {
  async fetch(req) {
    if (req.url.endsWith('/')) {
      return new Response(null, {
        status: 302,
        headers: {
          location: ' /\t2  ',
        },
      });
    } else if (req.url.endsWith('/2')) {
      return new Response('ok');
    }
  },
};

export const test = {
  async test(ctrl, env) {
    const resp = await env.SERVICE.fetch(
      ' http://p\tl\na\tc\ne\th\no\tl\nd\te\nr\t/ '
    );

    console.log(resp.url);

    strictEqual(await resp.text(), 'ok');
  },
};

export const typeTest = {
  test() {
    const resp = new Response('ok');
    strictEqual(resp.type, 'default');
  },
};
