import { strictEqual } from 'node:assert';

// TODO(cleanup): This is a copy of an existing test in streams-test. Once the autogate is remvoed,
// this separate test can be deleted.
export const test = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const input = new TextEncoder().encode('Hello, world!');
    const writePromise = writer.write(input);
    const closePromise = writer.close();
    const reader = its.readable.getReader();
    const { value, done } = await reader.read();
    strictEqual(done, false);
    strictEqual(value instanceof Uint8Array, true);
    strictEqual(value.length, input.length);
    for (let i = 0; i < input.length; i++) {
      strictEqual(value[i], input[i]);
    }
    await Promise.all([writePromise, closePromise]);
    const { done: doneAfterClose } = await reader.read();
    strictEqual(doneAfterClose, true);
  },
};
