import { strictEqual, ok, fail, deepStrictEqual } from 'node:assert';

async function expectResolution(promise) {
  try {
    return await promise;
  } catch {
    fail('error was not expected');
  }
}

async function expectRejection(promise, message) {
  try {
    await promise;
    fail('error was expected');
  } catch (err) {
    strictEqual(err.message, message);
  }
}

export const readAllTextFailedPull = {
  async test(ctrl, env) {
    const resp = await expectResolution(env.subrequest.fetch('http://example.org', {
      method: 'POST',
      body: new ReadableStream({
        async pull(c) {
          c.enqueue('test');
          await scheduler.wait(10);
          throw new Error('boom');
        },
      }, {
        highWaterMark: 0,
      }),
    }));
    await expectRejection(resp.text(), 'boom');
  },
};

export const readAllTextFailedStart = {
  async test(ctrl, env) {
    const resp = await expectResolution(env.subrequest.fetch('http://example.org', {
      method: 'POST',
      body: new ReadableStream({
        async start(c) {
          c.enqueue('test');
          await scheduler.wait(10);
          throw new Error('boom');
        },
      }, {
        highWaterMark: 0
      }),
    }));
    await expectRejection(resp.text(), 'boom');
  },
};

export const readAllTextFailed = {
  async test(ctrl, env) {
    const resp = await expectResolution(env.subrequest.fetch('http://example.org', {
      method: 'POST',
      body: new ReadableStream({
        async start(c) {
          c.enqueue('test');
          await scheduler.wait(10);
          c.error(new Error('boom'));
        },
      }, {
        highWaterMark: 0
      }),
    }));
    await expectRejection(resp.text(), 'boom');
  },
};

export const readableStreamFromAsyncGenerator = {
  async test(ctrl, env) {
    async function* gen() {
      yield 'test';
      await scheduler.wait(10);
      throw new Error('boom');
    }
    const resp = await expectResolution(env.subrequest.fetch('http://example.org', {
      method: 'POST',
      body: ReadableStream.from(gen()),
    }));
    await expectRejection(resp.text(), 'boom');
  },
};

export const readableStreamFromThrowingAsyncGen = {
  async test(ctrl, env) {
    async function* gen() {
      yield 'hello';
      await scheduler.wait(10);
      throw new Error('boom');
    }
    const resp = await expectResolution(env.subrequest.fetch('http://example.org', {
      method: 'POST',
      body: ReadableStream.from(gen()),
    }));
  },
};

export default {
  async fetch(request, env) {
    await request.text();
    request.signal.throwIfAborted();
    return new Response('OK');
  },
};
