import { WorkerEntrypoint } from 'cloudflare:workers';

import * as assert from 'node:assert';

export class JsRpcTester extends WorkerEntrypoint {
  async noArgs() {
    return 'hello from js';
  }
  async oneArg(a) {
    return `${a}`;
  }
  async identity(x) {
    return x;
  }
  async handleResponse(resp) {
    // Verify that we receive a JS object here...
    assert.deepStrictEqual(resp.constructor.name, 'Response');
    return resp;
  }

  async handleRequest(req) {
    assert.deepStrictEqual(req.constructor.name, 'Request');
    return req;
  }
}

export default {
  async test(ctrl, env, ctx) {
    // JS types
    for (const val of [
      1,
      'test',
      [1, 2, 3],
      new Map([['key', 42]]),
      42,
      1.2345,
      false,
      true,
      undefined,
    ]) {
      const response = await env.PythonRpc.identity(val);
      assert.deepStrictEqual(response, val);
    }

    const null_resp = await env.PythonRpc.identity(null);
    assert.ok(null_resp === null || null_resp === undefined);

    // Web/API Types
    const py_response = await env.PythonRpc.handle_response(
      new Response('this is a response')
    );
    assert.deepStrictEqual(await py_response.text(), 'this is a response');
    assert.equal(py_response.constructor.name, 'Response');

    const py_request = await env.PythonRpc.handle_request(
      new Request('https://test.com', { method: 'POST' })
    );
    assert.deepStrictEqual(py_request.method, 'POST');
    assert.equal(py_request.constructor.name, 'Request');
  },
};
