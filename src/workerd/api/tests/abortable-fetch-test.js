import { strictEqual, ok, throws } from 'node:assert';

// Test for the AbortSignal and AbortController standard Web API implementations.
// The implementation for these are in api/basics.{h|c++}

export const abortControllerAlreadyAborted = {
  async test(ctrl, env) {
    const ac = new AbortController();
    ac.abort();
    try {
      const result = await env.subrequest.fetch('http://example.org', {
        signal: ac.signal,
      });
      throw new Error('should have failed');
    } catch (err) {
      strictEqual(err.message, 'The operation was aborted');
    }
  },
};

export const alreadyAborted = {
  async test(ctrl, env) {
    const signal = AbortSignal.abort('boom');
    try {
      await env.subrequest.fetch('http://example.org', { signal });
      throw new Error('should have failed');
    } catch (err) {
      strictEqual(err, 'boom');
    }
  },
};

export const timedAbort = {
  async test(ctrl, env) {
    const signal = AbortSignal.timeout(100);
    try {
      await env.subrequest.fetch('http://example.org', { signal });
      throw new Error('should have failed');
    } catch (err) {
      strictEqual(err.message, 'The operation was aborted due to timeout');
    }
  },
};

export const abortControllerSyncAbort = {
  async test(ctrl, env) {
    const ac = new AbortController();
    try {
      const promise = env.subrequest.fetch('http://example.org', {
        signal: ac.signal,
      });
      ac.abort();
      await promise;
      throw new Error('should have failed');
    } catch (err) {
      strictEqual(err.message, 'The operation was aborted');
    }
  },
};

export const asyncSubrequest = {
  async test(ctrl, env) {
    try {
      await env.subrequest.fetch('http://example.org/sub');
      throw new Error('should have failed');
    } catch (err) {
      strictEqual(err.message, 'The operation was aborted due to timeout');
    }
  },
};

export const syncSubrequest = {
  async test(ctrl, env) {
    try {
      await env.subrequest.fetch('http://example.org/subsync');
      throw new Error('should have failed');
    } catch (err) {
      strictEqual(err.message, 'The operation was aborted');
    }
  },
};

export const requestAbortSignal = {
  test() {
    // The request objet has an AbortSignal, even if never used.
    const req1 = new Request('');
    ok(Reflect.has(req1, 'signal'));

    // request.signal should be the one passed in.
    const ac = new AbortController();
    const req2 = new Request('', { signal: ac.signal });
    strictEqual(req2.signal, ac.signal);
  },
};

export default {
  async fetch(request, env) {
    if (request.url.endsWith('/sub')) {
      // Verifies that a fetch subrequest returned as the response can be canceled
      // asynchronously successfully.
      const signal = AbortSignal.timeout(100);
      return env.subrequest.fetch('http://example.org', { signal });
    } else if (request.url.endsWith('/subsync')) {
      // Verifies that a fetch subrequest returned as the response can be synchronously
      // aborted.
      const ac = new AbortController();
      const resp = env.subrequest.fetch('http://example.org', {
        signal: ac.signal,
      });
      ac.abort();
      return resp;
    } else {
      await scheduler.wait(10000);
      return new Response('ok');
    }
  },
};
