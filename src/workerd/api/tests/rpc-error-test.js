import { notStrictEqual } from 'assert';

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
