import { rejects } from 'assert';

// Ported from the internal respond.ew-test

export const abortInternalStreamsTest = {
  async test(_, env) {
    await rejects(env.subrequest.fetch('http://example.org'), {
      message: /Body has already been used/,
    });
  },
};

export default {
  async fetch() {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(enc.encode('hello'));
      },
    });
    const response = new Response(rs);
    rs.cancel(new Error('boom'));
    return response;
  },
};
