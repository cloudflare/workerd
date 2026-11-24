// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The node worker in cluster-broken-node.wd-test answers `/own` itself, and
// `/thread` with the identity of its isolate (after `ms` milliseconds, if
// given): a node serving on several threads has an isolate per thread. A
// request whose `x-thread` header names another isolate it refuses with 409,
// `refused` and its own identity, before anything else is done with it, for
// the client to send it again until that isolate serves it; it holds the
// refusal for a while, so that the client's other attempts at once go to other
// threads, acts on only one of the attempts that share an `x-attempt`, and
// passes neither header on.
// `/<BINDING>/<path>` forwards `/<path>` to the bound service. This exposes
// failures from both a broken deployment service and a skipped deployment's
// absent service as JSON `{ error }` responses with status 500.
// `/BROKEN_DO/<name>` does the same for Durable Object `<name>` of the broken
// deployment's namespace, and adds the object's `id`.
let thread;

// The `x-attempt` of every request this isolate acted on.
const attempts = new Set();

export default {
  async fetch(request, env) {
    thread ??= crypto.randomUUID();
    const wanted = request.headers.get('x-thread');
    const attempt = request.headers.get('x-attempt');
    if (
      wanted !== null &&
      (wanted !== thread || (attempt !== null && attempts.has(attempt)))
    ) {
      await scheduler.wait(100);
      return Response.json({ thread, refused: true }, { status: 409 });
    }
    if (attempt !== null) attempts.add(attempt);
    request = new Request(request);
    request.headers.delete('x-thread');
    request.headers.delete('x-attempt');
    const url = new URL(request.url);
    const [, binding, ...path] = url.pathname.split('/');
    if (binding === 'own') {
      return Response.json({ own: true });
    }
    if (binding === 'thread') {
      await scheduler.wait(Number(url.searchParams.get('ms') ?? 0));
      return Response.json({ thread });
    }
    url.pathname = `/${path.join('/')}`;
    if (binding === 'BROKEN_DO') {
      const id = env.BROKEN_DO.idFromName(path[0]);
      try {
        return await env.BROKEN_DO.get(id).fetch(url, request);
      } catch (error) {
        const body = { error: String(error), id: id.toString() };
        return Response.json(body, { status: 500 });
      }
    }
    try {
      return await env[binding].fetch(url, request);
    } catch (error) {
      return Response.json({ error: String(error) }, { status: 500 });
    }
  },
};
