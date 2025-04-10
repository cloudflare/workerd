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

    // Web/API Types
    const py_response = await env.PythonRpc.handle_response(
      new Response('this is a response')
    );
    assert.deepStrictEqual(await py_response.text(), 'this is a response');

    const py_response2 = await env.PythonRpc.handle_request(
      new Request('https://test.com', { method: 'POST' })
    );
    assert.deepStrictEqual(py_response2.method, 'POST');
  },
};
