import { strictEqual } from 'assert';

export const abortInternalStreamsTest = {
  async test() {
    const { writable } = new IdentityTransformStream();

    const writer = writable.getWriter();

    const promise = writer.write(new Uint8Array(10));

    await writer.abort();

    // The write promise should abort proactively without waiting for a read,
    // indicating that the queue was drained proactively when the abort was
    // called.
    try {
      await promise;
      throw new Error('The promise should have been rejected');
    } catch (err) {
      strictEqual(err, undefined);
    }
  },
};
