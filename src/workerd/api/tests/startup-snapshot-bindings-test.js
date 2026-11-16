// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Capability bindings captured at module scope and used while handling a request. Libraries and
// user code commonly do this (`const db = drizzle(env.DB)`), so a Worker started from a startup
// snapshot must see the bindings its top level captured: the zygote compiles the real bindings
// into `env` and they transfer with it. A case Worker captures something at the top level and
// reports at request time whether it is the binding `env` holds and whether it works. Negative
// cases capture what cannot transfer, or have a binding of a kind that keeps the Worker out of
// the pipeline, and must start as plain Workers. The runner asserts the reports. Under the
// startup-snapshot autogate every service here goes through the zygote; without it they are plain
// Workers and the same assertions hold with snapshotUsed false.

import { strictEqual } from 'node:assert';
import { DurableObject, WorkerEntrypoint, env } from 'cloudflare:workers';
// Whether a Worker was started from a startup snapshot is asked at request time: at module scope
// the answer would be the zygote's and be baked into the snapshot.
import unsafe from 'workerd:unsafe';

const MODE = env.MODE;

// What the case's top level captures.
let captured;
switch (MODE) {
  case 'runner':
    break;
  case 'rpc':
    captured = env.RPC;
    break;
  case 'durable-object': {
    // The namespace and an ID minted from it: neither needs an IoContext.
    const ns = env.NS;
    captured = { ns, id: ns.idFromName('a') };
    break;
  }
  case 'rpc-property':
    // A method taken off a stub is a JsRpcProperty holding the stub, which has no transfer.
    captured = env.RPC.ping;
    break;
  case 'wrapped':
    // A wrapped binding is whatever JS an internal module returns; its kind keeps the Worker out
    // of the pipeline.
    captured = env.AI;
    break;
  case 'crypto-key':
    // A CryptoKey owns its key material in native objects; its kind keeps the Worker out of the
    // pipeline.
    captured = env.KEY;
    break;
  default:
    throw new Error(`unknown MODE ${MODE}`);
}

export class Rpc extends WorkerEntrypoint {
  ping() {
    return 'pong';
  }
}

export class Counter extends DurableObject {
  ping() {
    return 'pong';
  }
}

// What the captured binding looks like at request time. Every value must be true.
async function checks() {
  switch (MODE) {
    case 'rpc':
      return {
        sameBinding: captured === env.RPC,
        works: (await captured.ping()) === 'pong',
      };
    case 'durable-object':
      return {
        sameBinding: captured.ns === env.NS,
        sameId: captured.id.equals(env.NS.idFromName('a')),
        sameIdString:
          captured.id.toString() === env.NS.idFromName('a').toString(),
        works: (await captured.ns.get(captured.id).ping()) === 'pong',
      };
    case 'rpc-property':
      return { works: (await captured()) === 'pong' };
    case 'wrapped':
      return {
        sameBinding: captured === env.AI,
        isBinding: typeof captured.run === 'function',
      };
    case 'crypto-key':
      return {
        sameBinding: captured === env.KEY,
        isKey: captured.type === 'secret' && captured.algorithm.name === 'HMAC',
      };
  }
}

async function fetch() {
  return Response.json({
    mode: MODE,
    snapshotUsed: unsafe.startedFromStartupSnapshot(),
    checks: await checks(),
  });
}

async function test(ctrl, env) {
  // The runner went through the same pipeline as the cases: whether it was started from a
  // snapshot says whether the pipeline is on in this variant.
  const pipelineOn = unsafe.startedFromStartupSnapshot();

  // Cases whose top level the zygote can carry: started from the snapshot when the pipeline is
  // on.
  const transferable = ['rpc', 'durable-object'];
  // Cases that fail the zygote, or whose bindings keep them out of the pipeline: always plain
  // Workers.
  const fallback = ['rpc-property', 'wrapped', 'crypto-key'];

  const run = async (name, expectedSnapshotUsed) => {
    const response = await env[name].fetch('http://case/');
    const r = await response.json();
    strictEqual(r.mode, name);
    strictEqual(r.snapshotUsed, expectedSnapshotUsed, `${name}: snapshotUsed`);
    for (const [check, value] of Object.entries(r.checks)) {
      strictEqual(value, true, `${name}: ${check}`);
    }
  };
  for (const name of transferable) {
    await run(name, pipelineOn);
  }
  for (const name of fallback) {
    await run(name, false);
  }
}

// Only the runner has a test handler: workerd test runs test() of every service that exports one.
export default MODE === 'runner' ? { test } : { fetch };
