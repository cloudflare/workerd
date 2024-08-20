import { strictEqual, ok, deepStrictEqual, throws } from 'node:assert';

export const basics = {
  async test() {
    const response = Response.json({ a: 1 });
    ok(response instanceof Response);
    strictEqual(response.headers.get('content-type'), 'application/json');
    strictEqual(await response.text(), '{"a":1}');
  },
};

export const headers = {
  async test() {
    const response = Response.json(
      { a: 1 },
      {
        headers: [['content-type', 'abc']],
      }
    );
    ok(response instanceof Response);
    strictEqual(response.headers.get('content-type'), 'abc');
    deepStrictEqual(await response.json(), { a: 1 });
  },
};

export const headers2 = {
  async test() {
    const headers = new Headers();
    headers.set('content-type', 'abc');
    const response = Response.json({ a: 1 }, { headers });
    ok(response instanceof Response);
    strictEqual(response.headers.get('content-type'), 'abc');
    deepStrictEqual(await response.json(), { a: 1 });
  },
};

export const headers3 = {
  async test() {
    const other = new Response();
    other.headers.set('content-type', 'abc');
    const response = Response.json({ a: 1 }, other);
    ok(response instanceof Response);
    strictEqual(response.headers.get('content-type'), 'abc');
    deepStrictEqual(await response.json(), { a: 1 });
  },
};

export const headers4 = {
  async test() {
    const response = Response.json({ a: 1 }, {});
    ok(response instanceof Response);
    strictEqual(response.headers.get('content-type'), 'application/json');
    deepStrictEqual(await response.json(), { a: 1 });
  },
};

export const notJsonSerializable = {
  async test() {
    // Some values are not JSON serializable...
    throws(() => Response.json({ a: 1n }));
  },
};
