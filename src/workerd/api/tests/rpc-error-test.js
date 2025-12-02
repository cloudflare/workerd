import { notStrictEqual, ok, strictEqual, throws } from 'assert';

export const test = {
  async test(_, env) {
    try {
      await env.RPC.foo(null);
      throw new Error('Expected error not thrown');
    } catch (e) {
      // The error returned by RPC will have a stack2 property that contains
      // the original stack from the worker where the error was thrown.
      notStrictEqual(e.stack2, undefined);
      // Because we are not setting the trusted option when deserializing the
      // error, the stack of the error reported here should be different from
      // the original stack.
      notStrictEqual(e.stack2, e.stack);
    }

    try {
      const bar = await env.RPC.bar(null);
      const baz = await bar.baz(null);
      await baz.qux(null);
      throw new Error('Expected error not thrown');
    } catch (e) {
      // The error returned by RPC will have a stack2 property that contains
      // the original stack from the worker where the error was thrown.
      notStrictEqual(e.stack2, undefined);
      // Because we are not setting the trusted option when deserializing the
      // error, the stack of the error reported here should be different from
      // the original stack.
      notStrictEqual(e.stack2, e.stack);
    }

    try {
      await env.RPC.xyz(null);
      throw new Error('Expected error not thrown');
    } catch (e) {
      // The error returned by RPC will have a stack2 property that contains
      // the original stack from the worker where the error was thrown.
      notStrictEqual(e.stack2, undefined);
      // Because we are not setting the trusted option when deserializing the
      // error, the stack of the error reported here should be different from
      // the original stack.
      notStrictEqual(e.stack2, e.stack);
    }

    {
      const err = await env.RPC.abc(null).a;
      // The error returned by RPC will have a stack2 property that contains
      // the original stack from the worker where the error was thrown.
      notStrictEqual(err.stack2, undefined);
      // Because we are not setting the trusted option when deserializing the
      // error, the stack of the error reported here should be different from
      // the original stack.
      notStrictEqual(err.stack2, err.stack);
    }
  },
};

export let structuredCloneNotBroken = {
  test(controller, env, ctx) {
    // At one point, enhanced_error_serialization inadvertently broke structuredClone()'s support
    // for host objects.
    let orig = new Headers({ foo: '123', bar: 'abc' });
    let cloned = structuredClone(orig);
    ok(cloned instanceof Headers);
    strictEqual(cloned.get('foo'), '123');
    strictEqual(cloned.get('bar'), 'abc');

    throws(() => structuredClone(new TextEncoder()), {
      name: 'DataCloneError',
      code: DOMException.DATA_CLONE_ERR,
      message:
        'Could not serialize object of type "TextEncoder". This type does not support ' +
        'serialization.',
    });
  },
};

// DOMException is structured cloneable
export let domExceptionClone = {
  test() {
    const de1 = new DOMException('hello', 'NotAllowedError');

    de1.foo = 'abc';

    const de2 = structuredClone(de1);
    ok(de2 instanceof DOMException);
    strictEqual(de1.name, de2.name);
    strictEqual(de1.message, de2.message);
    strictEqual(de1.stack, de2.stack);
    strictEqual(de1.code, de2.code);
    notStrictEqual(de1, de2);

    // TODO(bug): This is supposed to work, but currently doesn't because the "native error"
    //   handling in jsg::Serializer does not kick in for DOMException. Instead, DOMException's
    //   own serializer (in dom-exception.c++) is used, and it hasn't been updated to handle
    //   serializing application properties.
    // strictEqual(de1.foo, de2.foo);
    // strictEqual(de2.foo, 'abc');
  },
};
