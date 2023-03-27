import {
  notStrictEqual,
  strictEqual,
} from 'node:assert';

export const identityTransformStream = {
  async test(ctrl, env, ctx) {
    const ts = new IdentityTransformStream({ highWaterMark: 10 });
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    strictEqual(writer.desiredSize, 10);

    // We shouldn't have to wait here.
    const firstReady = writer.ready;
    await writer.ready;

    writer.write(new Uint8Array(1));
    strictEqual(writer.desiredSize, 9);

    // Let's write a second chunk that will be buffered. This one
    // should impact the desiredSize and the backpressure signal.
    writer.write(new Uint8Array(9));
    strictEqual(writer.desiredSize, 0);

    // The ready promise should have been replaced
    notStrictEqual(firstReady, writer.ready);

    async function waitForReady() {
      strictEqual(writer.desiredSize, 0);
      await writer.ready;
      // The backpressure should have been relieved a bit,
      // but only by the amount of what we've currently read.
      strictEqual(writer.desiredSize, 1);
    }

    await Promise.all([
      // We call the waitForReady first to ensure that we set up waiting on
      // the ready promise before we relieve the backpressure using the read.
      // If the backpressure signal is not working correctly, the test will
      // fail with an error indicating that a hanging promise was canceled.
      waitForReady(),
      reader.read(),
    ]);

    // If we read again, the backpressure should be fully resolved.
    await reader.read();
    strictEqual(writer.desiredSize, 10);
  }
};

export const identityTransformStreamNoHWM = {
  async test(ctrl, env, ctx) {
    // Test that the original default behavior still works as expected.

    const ts = new IdentityTransformStream();
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    strictEqual(writer.desiredSize, 1);

    // We shouldn't have to wait here.
    const firstReady = writer.ready;
    await writer.ready;

    writer.write(new Uint8Array(1));
    strictEqual(writer.desiredSize, 1);

    // Let's write a second chunk that will be buffered. There should
    // be no indication that the desired size has changed.
    writer.write(new Uint8Array(9));
    strictEqual(writer.desiredSize, 1);

    // The ready promise should be exactly the same...
    strictEqual(firstReady, writer.ready);

    async function waitForReady() {
      strictEqual(writer.desiredSize, 1);
      await writer.ready;
      strictEqual(writer.desiredSize, 1);
    }

    await Promise.all([
      // We call the waitForReady first to ensure that we set up waiting on
      // the ready promise before we relieve the backpressure using the read.
      // If the backpressure signal is not working correctly, the test will
      // fail with an error indicating that a hanging promise was canceled.
      waitForReady(),
      reader.read(),
    ]);

    // If we read again, the backpressure should be fully resolved.
    await reader.read();
    strictEqual(writer.desiredSize, 1);
  }
};

export const fixedLengthStream = {
  async test(ctrl, env, ctx) {
    const ts = new FixedLengthStream(10, { highWaterMark: 100 });
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    // Even tho we specified 100 as our highWaterMark, we only expect 10
    // bytes total, so we'll make that our highWaterMark instead.
    strictEqual(writer.desiredSize, 10);

    // We shouldn't have to wait here.
    const firstReady = writer.ready;
    await writer.ready;

    writer.write(new Uint8Array(1));
    strictEqual(writer.desiredSize, 9);

    // Let's write a second chunk that will be buffered. This one
    // should impact the desiredSize and the backpressure signal.
    writer.write(new Uint8Array(9));
    strictEqual(writer.desiredSize, 0);

    // The ready promise should have been replaced
    notStrictEqual(firstReady, writer.ready);

    async function waitForReady() {
      await writer.ready;
      // The backpressure should have been relieved a bit,
      // but only by the amount of what we've currently read.
      strictEqual(writer.desiredSize, 1);
    }

    await Promise.all([
      // We call the waitForReady first to ensure that we set up waiting on
      // the ready promise before we relieve the backpressure using the read.
      waitForReady(),
      reader.read(),
    ]);

    // If we read again, the backpressure should be fully resolved.
    await reader.read();
    strictEqual(writer.desiredSize, 10);
  }
};
