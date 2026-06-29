// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

// The backend service that the wrapped binding's inner stub points at.
export const backend = {
  async fetch(req) {
    return new Response('pong');
  },
};

// An RPC entrypoint used to verify that a wrapped binding survives being:
//   1. passed as an RPC argument (deserialized via RpcDeserializerExternalHandler), and
//   2. embedded in `ctx.props` (deserialized via Frankenvalue::CapTableReader).
export class Receiver extends WorkerEntrypoint {
  async useDoor(door) {
    // `door` was reconstructed from an RPC argument. It should be a fully-functional wrapped
    // binding: the wrapper-supplied `label` is present and the inner stub still works.
    assert.strictEqual(
      door.self,
      door,
      'constructor-created self reference should be preserved'
    );
    return `${door.label}:${await door.ping()}`;
  }

  async usePropsDoor() {
    // `this.ctx.props.door` was reconstructed from a Frankenvalue.
    const door = this.ctx.props.door;
    return `${door.label}:${await door.ping()}`;
  }

  // Wrapped binding nested inside the argument graph (depth > 1).
  async useNestedDoor(payload) {
    return `${payload.outer.inner.door.label}:${await payload.outer.inner.door.ping()}`;
  }

  // Wrapped binding in an argument graph that also contains a cycle.
  async useCyclicDoor(payload) {
    assert.strictEqual(payload.self, payload, 'cycle should round-trip intact');
    return `${payload.door.label}:${await payload.door.ping()}`;
  }

  async useDoorFromMap(payload) {
    const door = payload.entries.get('db');
    return `${door.label}:${await door.ping()}`;
  }
}

// Sanity check: the wrapped binding works locally before any serialization.
export const test_local = {
  async test(ctrl, env, ctx) {
    assert.strictEqual(typeof env.DOOR, 'object');
    assert.strictEqual(env.DOOR.self, env.DOOR);
    assert.strictEqual(env.DOOR.label, 'wrapped-door');
    assert.strictEqual(await env.DOOR.ping(), 'pong');
  },
};

// Pass the wrapped binding as an RPC argument to another worker, which reconstructs it and calls
// through it. Exercises the RPC serialize/deserialize path.
export const test_rpc_argument = {
  async test(ctrl, env, ctx) {
    const result = await env.RECEIVER.useDoor(env.DOOR);
    assert.strictEqual(result, 'wrapped-door:pong');
  },
};

// Embed the wrapped binding in `ctx.props` of a stub. Props are encoded as a Frankenvalue, so this
// exercises the Frankenvalue serialize/deserialize path.
export const test_frankenvalue_props = {
  async test(ctrl, env, ctx) {
    const stub = ctx.exports.Receiver({ props: { door: env.DOOR } });
    const result = await stub.usePropsDoor();
    assert.strictEqual(result, 'wrapped-door:pong');
  },
};

// Pass a wrapped binding nested deep inside an argument structure.
export const test_rpc_deeply_nested = {
  async test(ctrl, env, ctx) {
    const result = await env.RECEIVER.useNestedDoor({
      outer: { inner: { door: env.DOOR } },
    });
    assert.strictEqual(result, 'wrapped-door:pong');
  },
};

// Pass a cyclic argument graph (structured-clone supports cycles) containing a wrapped binding.
export const test_rpc_cyclic_arg = {
  async test(ctrl, env, ctx) {
    const payload = { door: env.DOOR };
    payload.self = payload;
    const result = await env.RECEIVER.useCyclicDoor(payload);
    assert.strictEqual(result, 'wrapped-door:pong');
  },
};

// Pass a wrapped binding stored as a value inside a Map.
export const test_rpc_inside_map = {
  async test(ctrl, env, ctx) {
    const payload = { entries: new Map([['db', env.DOOR]]) };
    const result = await env.RECEIVER.useDoorFromMap(payload);
    assert.strictEqual(result, 'wrapped-door:pong');
  },
};
