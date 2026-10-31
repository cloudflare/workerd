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
//   /get             Returns the current counter value without incrementing it,
//                    along with `restores`, the number of times this DO's
//                    [restore]() method has run.
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
//   /call-held-stub  Calls /identity on the stub kept by /hold-stub for the DO
//                    named by the `target` query param. Returns the target's
//                    response.
//   /hold            Waits one second before responding, so that the test can
//                    break the DO while this request is still in flight.
//   /break           Aborts the DO via ctx.abort(). The request fails.
//   /set-alarm       Sets the DO's alarm `ms` (default 60000) from now.
//   /alarms          Returns `fired`, the NODE_ID of each instance that ran the
//                    DO's alarm, in order.
//   /arm-constructor-failure
//                    Sets a storage flag that makes the *next* instantiation of
//                    this DO throw from its constructor (the flag is cleared and
//                    committed before throwing, so the instantiation after that
//                    succeeds), then aborts the DO so that the next request
//                    re-instantiates it.
//
// Persistent-stub paths. Kind `rpc` names a CounterAdder RpcTarget, vended by
// the DO given by the `target` query param via its [restore]() method, that
// mutates the target's counter:
//   /vend-stub?kind=K&target=T
//                    Asks the target (over RPC) to vend a persistent stub of the
//                    given kind, stores it in this DO's storage under `kind`, and
//                    reads it back without calling it.
//   /use-stub?kind=K[&amount=N]
//                    Loads the stub stored by /vend-stub and calls add(amount)
//                    on it. Returns the result.
//   /vend-to-consumer
//                    Vends an `rpc` stub of *this* DO and hands it, via props,
//                    to a Consumer entrypoint in the same process, which keeps
//                    only the stub's channel (props carry no live capability).
//                    The Consumer waits until DO "sink" has its `go` flag set,
//                    then calls the stub and records the result in "sink".
//   /set-go          Sets this DO's `go` flag (read by Consumer via /get).
//   /record?value=V  Stores V (JSON) as this DO's `recorded` value.
//
// The top-level fetch() additionally honors a `delay` query parameter (ms): it
// waits that long *before* calling the DO. This lets a test put a request in
// flight on a node and SIGTERM that node while the request has not yet routed
// to the DO's owner.

import {
  DurableObject,
  RpcTarget,
  WorkerEntrypoint,
  restore,
} from 'cloudflare:workers';

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

    const delay = url.searchParams.get('delay');
    if (delay !== null) {
      await scheduler.wait(Number(delay));
    }

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

// RpcTarget vended by Counter's [restore]() that mutates the counter's storage.
// It runs in the Counter's context on whichever node owns the Counter.
class CounterAdder extends RpcTarget {
  constructor(counter) {
    super();
    this.counter = counter;
  }

  async add(amount) {
    const count =
      ((await this.counter.state.storage.get('count')) ?? 0) + amount;
    await this.counter.state.storage.put('count', count);
    return { count, nodeId: this.counter.env.NODE_ID ?? '<unknown>' };
  }
}

// See /vend-to-consumer. The stub in `props` was minted by ctx.restore() in this
// process and arrives here without its live capability, so the first call on it
// restores through the channel object the minting DO created, not through a
// decoded token.
export class Consumer extends WorkerEntrypoint {
  useLater() {
    const adder = this.ctx.props.adder;
    const sink = this.env.COUNTER.get(this.env.COUNTER.idFromName('sink'));
    this.ctx.waitUntil(
      (async () => {
        for (;;) {
          const state = await (await sink.fetch('http://do/get')).json();
          if (state.go) break;
          await scheduler.wait(50);
        }
        let result;
        try {
          result = await adder.add(1);
        } catch (err) {
          result = { error: String(err) };
        }
        await sink.fetch(
          'http://do/record?value=' + encodeURIComponent(JSON.stringify(result))
        );
      })()
    );
  }
}

const BREAK_ON_CONSTRUCT = 'break-on-construct';

export class Counter extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.state = state;
    this.env = env;
    // See /hold-stub. Maps target DO name -> stub.
    this.heldStubs = new Map();

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
      const restores = (await this.state.storage.get('restores')) ?? 0;
      const go = (await this.state.storage.get('go')) ?? false;
      const recorded = await this.state.storage.get('recorded');
      return Response.json({
        count,
        restores,
        go,
        recorded,
        nodeId,
        id: idHex,
      });
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
      this.heldStubs.set(target, stub);
      const response = await stub.fetch('http://do/identity');
      return Response.json({
        nodeId,
        id: idHex,
        target: await response.json(),
      });
    } else if (url.pathname === '/call-held-stub') {
      const target = url.searchParams.get('target');
      const stub = this.heldStubs.get(target);
      if (stub === undefined) {
        return Response.json({ error: 'no held stub' }, { status: 400 });
      }
      const response = await stub.fetch('http://do/identity');
      return Response.json({
        nodeId,
        id: idHex,
        target: await response.json(),
      });
    } else if (url.pathname === '/vend-stub') {
      const kind = url.searchParams.get('kind');
      const target = url.searchParams.get('target');
      const ns = this.state.exports.Counter;
      const stub = await ns.get(ns.idFromName(target)).vend(kind);
      await this.state.storage.put(kind, stub);
      // Read it back so that the stub is also deserialized, but do not call it.
      const loaded = await this.state.storage.get(kind);
      return Response.json({
        nodeId,
        id: idHex,
        hasStub: loaded !== undefined,
      });
    } else if (url.pathname === '/use-stub') {
      const kind = url.searchParams.get('kind');
      const stub = await this.state.storage.get(kind);
      const result = await stub.add(
        Number(url.searchParams.get('amount') ?? '1')
      );
      return Response.json({ nodeId, id: idHex, result });
    } else if (url.pathname === '/vend-to-consumer') {
      const adder = await this.state.restore({ kind: 'rpc' });
      const consumer = this.state.exports.Consumer({ props: { adder } });
      await consumer.useLater();
      return Response.json({ nodeId, id: idHex });
    } else if (url.pathname === '/set-go') {
      await this.state.storage.put('go', true);
      return Response.json({ nodeId, id: idHex });
    } else if (url.pathname === '/record') {
      await this.state.storage.put(
        'recorded',
        JSON.parse(url.searchParams.get('value'))
      );
      return Response.json({ nodeId, id: idHex });
    } else if (url.pathname === '/hold') {
      await scheduler.wait(1000);
      return Response.json({ nodeId, id: idHex });
    } else if (url.pathname === '/set-alarm') {
      const ms = Number(url.searchParams.get('ms') ?? '60000');
      await this.state.storage.setAlarm(Date.now() + ms);
      return Response.json({ nodeId, id: idHex });
    } else if (url.pathname === '/alarms') {
      const fired = (await this.state.storage.get('fired')) ?? [];
      return Response.json({ nodeId, id: idHex, fired });
    } else if (url.pathname === '/break') {
      this.state.abort('broken on purpose');
    } else if (url.pathname === '/arm-constructor-failure') {
      await this.state.storage.put(BREAK_ON_CONSTRUCT, true);
      await this.state.storage.sync();
      this.state.abort('armed constructor failure');
    }

    return new Response('Not found', { status: 404 });
  }

  // Records which node ran each alarm.
  async alarm() {
    const fired = (await this.state.storage.get('fired')) ?? [];
    fired.push(this.env.NODE_ID);
    await this.state.storage.put('fired', fired);
  }

  // RPC method used by /vend-stub on the *target* DO.
  vend(kind) {
    return this.state.restore({ kind });
  }

  async [restore](params) {
    const restores = ((await this.state.storage.get('restores')) ?? 0) + 1;
    await this.state.storage.put('restores', restores);

    if (params.kind === 'rpc') return new CounterAdder(this);
    throw new Error(`unexpected restore params: ${JSON.stringify(params)}`);
  }
}
