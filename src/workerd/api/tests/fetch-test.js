import { strictEqual, throws } from 'assert';

// Test depends on the setting of the upper_case_all_http_methods compatibility flag.
strictEqual(
  globalThis.Cloudflare.compatibilityFlags['upper_case_all_http_methods'],
  true
);

export const test = {
  test() {
    // Verify that lower-cased method names are converted to upper-case.
    // even though the Fetch API doesn't do this in general for all methods.
    // Note that the upper_case_all_http_methods compat flag is intentionally
    // diverging from the Fetch API here.
    const req = new Request('https://example.com', { method: 'patch' });
    strictEqual(req.method, 'PATCH');

    // Unrecognized methods error as expected, with the error message
    // showing the original-cased method name.
    throws(() => new Request('http://example.org', { method: 'patchy' }), {
      message: /^Invalid HTTP method string: patchy$/,
    });
  },
};

export const fetchGen = {
  async test() {
    const enc = new TextEncoder();
    async function* gen() {
      yield enc.encode('Hello ');
      yield enc.encode('World!');
    }
    const resp = new Response(gen());
    strictEqual(await resp.text(), 'Hello World!');

    const req = new Request('http://example.com', {
      method: 'POST',
      body: gen(),
    });
    strictEqual(await req.text(), 'Hello World!');

    const resp2 = new Response([enc.encode('Hello '), enc.encode('World!')]);
    strictEqual(await resp2.text(), 'Hello World!');

    // Boxed strings work correctly.
    const resp3 = new Response(new String('Hello'));
    strictEqual(await resp3.text(), 'Hello');

    // Regular strings work correctly.
    const resp4 = new Response('Hello');
    strictEqual(await resp4.text(), 'Hello');

    const objIter = {
      *[Symbol.iterator]() {
        yield enc.encode('Hello ');
        yield enc.encode('Object Iterator!');
      },
    };
    const respIter = new Response(objIter);
    strictEqual(await respIter.text(), 'Hello Object Iterator!');

    // Custom toString prevents iterator treatment.
    const obj = {
      *[Symbol.iterator]() {
        yield enc.encode('ignored');
      },
      toString() {
        return 'Hello';
      },
    };
    const resp5 = new Response(obj);
    strictEqual(await resp5.text(), 'Hello');

    class MyObj {
      *[Symbol.iterator]() {
        yield enc.encode('ignored');
      }
      toString() {
        return 'Hello';
      }
    }
    const resp6 = new Response(new MyObj());
    strictEqual(await resp6.text(), 'Hello');

    class Base {
      toString() {
        return 'Hello';
      }
    }
    class SubObj extends Base {
      *[Symbol.iterator]() {
        yield enc.encode('ignored');
      }
    }
    const resp7 = new Response(new SubObj());
    strictEqual(await resp7.text(), 'Hello');

    // Also custom toPrimitive prevents iterator treatment.
    const objPrim = {
      *[Symbol.iterator]() {
        yield enc.encode('ignored');
      },
      [Symbol.toPrimitive]() {
        return 'Hello';
      },
    };
    const respPrim = new Response(objPrim);
    strictEqual(await respPrim.text(), 'Hello');

    // Unless it's specifically an async iterator.
    const asyncIter = {
      async *[Symbol.asyncIterator]() {
        yield enc.encode('Hello ');
        yield enc.encode('Async World!');
      },
      toString() {
        return 'ignored';
      },
    };
    const resp8 = new Response(asyncIter);
    strictEqual(await resp8.text(), 'Hello Async World!');
  },
};
