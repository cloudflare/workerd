import { strictEqual, rejects, throws } from 'node:assert';

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
    } else if (req.url.endsWith('/error')) {
      const resp = Response.error();
      strictEqual(resp.type, 'error');
      strictEqual(resp.status, 0);
      return resp;
    }
  },

  foo() {
    return Response.error();
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

export const error = {
  async test(_, env) {
    await rejects(env.SERVICE.fetch('http://example/error'), {
      message:
        'Return value from serve handler must not be an error ' +
        'response (like Response.error())',
    });

    const errorResp = Response.error();
    throws(() => new Response('', errorResp), {
      message:
        'Responses may only be constructed with status codes in ' +
        'the range 200 to 599, inclusive.',
    });

    const errorRespRpc = await env.SERVICE.foo(null);
    strictEqual(errorRespRpc.type, 'error');
    strictEqual(errorRespRpc.ok, false);
  },
};
