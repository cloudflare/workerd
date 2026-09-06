// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Test worker for the cluster integration test. Exposes a Counter Durable Object
// and a top-level fetch() that routes requests to the DO by name. Requests that
// fail are reported as JSON `{ error }` with status 500 rather than letting the
// exception propagate, so the test can assert on the message.
//
// The DO supports the following request paths:
//   /increment       Increments and returns the current counter value.
//   /get             Returns the current counter value without incrementing it.
//   /set-alarm       Tries to schedule a DO alarm. In cluster mode this should
//                    fail with a "not yet supported" error.
//   /identity        Returns information identifying this instance (NODE_ID env)
//                    along with the DO id, so the test can verify which instance
//                    served the request.
//   /store-stub      Stores a stub to the DO named by the `target` query param in
//                    this DO's storage, then reads it back without calling it.
//                    Returns the target's id.
//   /call-stored-stub
//                    Loads the stub stored by /store-stub and calls /get on it.
//                    Returns the target's response.
//   /hold-stub       Obtains a stub to the DO named by the `target` query param,
//                    calls /identity on it, and keeps the stub on this instance
//                    for as long as this instance lives. (A stub is owned by the
//                    request context that created it, so a module-level variable
//                    would not keep it alive past the request; an instance field
//                    of a long-lived DO does.) Returns the target's response.
//   /hold            Waits one second before responding, so that the test can
//                    break the DO while this request is still in flight.
//   /break           Aborts the DO via ctx.abort(). The request fails.
//   /arm-constructor-failure
//                    Sets a storage flag that makes the *next* instantiation of
//                    this DO throw from its constructor (the flag is cleared and
//                    committed before throwing, so the instantiation after that
//                    succeeds), then aborts the DO so that the next request
//                    re-instantiates it.

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    // The DO name to act on is given via the `name` query parameter (defaults
    // to "default"). All instances using the same name will route to the same
    // DO id.
    const name = url.searchParams.get('name') ?? 'default';

    // Synthesize a stub URL for the DO.
    const id = env.COUNTER.idFromName(name);
    const stub = env.COUNTER.get(id);

    // Forward to the DO with the same path and query.
    const forwardUrl = new URL(url.pathname + url.search, 'http://do/');
    try {
      return await stub.fetch(forwardUrl.toString(), {
        method: request.method,
        headers: request.headers,
      });
    } catch (err) {
      return Response.json({ error: String(err) }, { status: 500 });
    }
  },
};

const BREAK_ON_CONSTRUCT = 'break-on-construct';

export class Counter {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    // See /hold-stub.
    this.heldStubs = [];

    state.blockConcurrencyWhile(async () => {
      if (await state.storage.get(BREAK_ON_CONSTRUCT)) {
        // One-shot: make sure the cleared flag is durable before breaking, so
        // the next instantiation succeeds.
        await state.storage.delete(BREAK_ON_CONSTRUCT);
        await state.storage.sync();
        throw new Error('constructor failed on purpose');
      }
    });
  }

  async fetch(request) {
    const url = new URL(request.url);
    const nodeId = this.env.NODE_ID ?? '<unknown>';
    const idHex = this.state.id.toString();

    if (url.pathname === '/increment') {
      let count = (await this.state.storage.get('count')) ?? 0;
      count += 1;
      await this.state.storage.put('count', count);
      return Response.json({
        count,
        nodeId,
        id: idHex,
      });
    } else if (url.pathname === '/get') {
      const count = (await this.state.storage.get('count')) ?? 0;
      return Response.json({ count, nodeId, id: idHex });
    } else if (url.pathname === '/set-alarm') {
      // Try to schedule an alarm 60s in the future. In cluster mode this should
      // throw a clear error.
      try {
        await this.state.storage.setAlarm(Date.now() + 60_000);
        return Response.json({ ok: true, nodeId, id: idHex });
      } catch (err) {
        return Response.json(
          { ok: false, error: String(err), nodeId, id: idHex },
          { status: 500 }
        );
      }
    } else if (url.pathname === '/identity') {
      return Response.json({ nodeId, id: idHex });
    } else if (url.pathname === '/store-stub') {
      // Only stubs from the ctx.exports self-binding are storable.
      const target = url.searchParams.get('target');
      const ns = this.state.exports.Counter;
      const targetId = ns.idFromName(target);
      await this.state.storage.put('stub', ns.get(targetId));
      // Read it back so that the stub is also deserialized, but do not call it.
      const stub = await this.state.storage.get('stub');
      return Response.json({
        nodeId,
        id: idHex,
        targetId: targetId.toString(),
        hasStub: typeof stub?.fetch === 'function',
      });
    } else if (url.pathname === '/call-stored-stub') {
      const stub = await this.state.storage.get('stub');
      const response = await stub.fetch('http://do/get');
      return Response.json({
        nodeId,
        id: idHex,
        target: await response.json(),
      });
    } else if (url.pathname === '/hold-stub') {
      const target = url.searchParams.get('target');
      const stub = this.env.COUNTER.get(this.env.COUNTER.idFromName(target));
      this.heldStubs.push(stub);
      const response = await stub.fetch('http://do/identity');
      return Response.json({
        nodeId,
        id: idHex,
        target: await response.json(),
      });
    } else if (url.pathname === '/hold') {
      await scheduler.wait(1000);
      return Response.json({ nodeId, id: idHex });
    } else if (url.pathname === '/break') {
      this.state.abort('broken on purpose');
    } else if (url.pathname === '/arm-constructor-failure') {
      await this.state.storage.put(BREAK_ON_CONSTRUCT, true);
      await this.state.storage.sync();
      this.state.abort('armed constructor failure');
    }

    return new Response('Not found', { status: 404 });
  }

  async alarm() {
    // Should never be called in cluster mode; if it ever is, surface that via
    // a stored marker so the test can observe the failure mode.
    await this.state.storage.put('alarm-fired', true);
  }
}
