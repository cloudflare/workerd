// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Live API objects across a startup snapshot. A case Worker's top level creates objects whose
// native side has to travel through the snapshot (Headers, URL, TextEncoder, crypto, navigator,
// the text/data/JSON bindings; node:fs in startup-snapshot-wrappables-fs-test.js) and at request
// time reports whether they still hold their state and still work. Negative cases leave an object
// the snapshot cannot carry reachable, or do not qualify for the pipeline, and must start as plain
// Workers. The runner asserts the reports. Under the startup-snapshot autogate every service here
// goes through the zygote; without it they are plain Workers and the same assertions hold with
// snapshotUsed false.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { DurableObject, WorkerEntrypoint, env } from 'cloudflare:workers';
// Whether a Worker was started from a startup snapshot is asked at request time: at module scope
// the answer would be the zygote's and be baked into the snapshot.
import unsafe from 'workerd:unsafe';

const MODE = env.MODE;

// Top-level state every case creates. Each object is a JSG resource whose C++ side the snapshot
// carries either as a snapshotClone() (Headers, URL) or by recreating a stateless type
// (TextEncoder, Crypto, SubtleCrypto, Navigator).
const headers = new Headers({ 'x-a': '1', 'content-type': 'text/plain' });
headers.append('x-a', '2');
const url = new URL('https://example.com/p/a/t/h?query=string&x=1#hash');
const encoder = new TextEncoder();
const cryptoAtTopLevel = crypto;
const subtle = crypto.subtle;
const navigatorAtTopLevel = navigator;
// The text/data/JSON bindings are baked into the snapshot together with env itself.
const textAtTopLevel = env.TEXT;
const jsonAtTopLevel = env.JSON;
const dataAtTopLevel = env.DATA;

// Per-case top-level state.
let controller = null;
let params = null;
let kept = null;
// The finalization case's registry and what its cleanup callback did. The handler reads this
// object, which keeps the registry reachable: an unreachable registry never runs its callback.
const finalization = { registry: null, cleaned: [], headers: null };

switch (MODE) {
  case 'unclonable':
    // Neither stateless nor clonable: the zygote rejects the snapshot.
    controller = new AbortController();
    break;
  case 'searchparams':
    // URLSearchParams is not transferable either. URL::snapshotClone() drops the one it
    // created lazily, but a reference the top level keeps makes it reachable.
    params = url.searchParams;
    break;
  case 'gc':
    // Wrappers created and dropped before the snapshot: their collection in the zygote must not
    // disturb what is transferred.
    churn();
    kept = new Headers({ kept: 'yes' });
    collect();
    break;
  case 'finalization':
    // A FinalizationRegistry cleanup the top level leaves due: its target is garbage as soon as
    // it is registered. The zygote runs the callback before recording anything, so what it
    // creates travels with the snapshot. No collect() here: finding the target is the
    // pipeline's job.
    finalization.registry = new FinalizationRegistry((held) => {
      finalization.cleaned.push(held);
      finalization.headers = new Headers({ 'x-held': held });
    });
    finalization.registry.register({}, 'b');
    break;
}

// Creates and drops wrappers, then collects them.
function churn() {
  for (let i = 0; i < 50; i++) {
    new Headers({ n: String(i) });
    new URL(`https://example.com/${i}`);
    new TextEncoder();
  }
  collect();
}

// Collects garbage. The zygote has no gc(): V8 installs the --expose-gc extension only in an
// isolate that is not building a snapshot. There, external memory pressure makes V8 start a
// major collection and, once the allocation limit is overshot, finish it synchronously.
function collect() {
  if (typeof gc === 'function') {
    gc();
    return;
  }
  for (let i = 0; i < 8; i++) new ArrayBuffer(16 << 20);
}

// What the shared top-level state looks like at request time. Every value must be true.
async function checks() {
  const keys = [...headers.keys()];
  headers.set('x-b', '3');
  const hrefAtTopLevel = url.href;
  url.hash = '#other';
  const digest = await subtle.digest('SHA-256', encoder.encode('abc'));
  const hex = [...new Uint8Array(digest)]
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
  const result = {
    headersGet: headers.get('x-a') === '1, 2',
    headersKeys: keys.join() === 'content-type,x-a',
    headersMutable: headers.get('x-b') === '3',
    urlHref:
      hrefAtTopLevel === 'https://example.com/p/a/t/h?query=string&x=1#hash',
    urlSearchParams: url.searchParams.get('query') === 'string',
    urlMutable:
      url.href === 'https://example.com/p/a/t/h?query=string&x=1#other',
    encoder: encoder.encode('héllo').length === 6,
    randomUUID: /^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$/.test(
      cryptoAtTopLevel.randomUUID()
    ),
    digest:
      hex ===
      'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
    userAgent: navigatorAtTopLevel.userAgent === 'Cloudflare-Workers',
    textBinding: env.TEXT === 'hello' && env.TEXT === textAtTopLevel,
    jsonBinding: env.JSON === jsonAtTopLevel && env.JSON.answer === 42,
    dataBinding:
      env.DATA === dataAtTopLevel &&
      env.DATA instanceof ArrayBuffer &&
      new Uint8Array(env.DATA).join() === '97,98,99',
  };
  switch (MODE) {
    case 'unclonable':
      result.controller = !controller.signal.aborted;
      break;
    case 'searchparams':
      result.params = params.get('query') === 'string';
      break;
    case 'gc':
      collect();
      result.keptAfterGc = kept.get('kept') === 'yes';
      churn();
      result.keptAfterChurn = kept.get('kept') === 'yes';
      break;
    case 'finalization': {
      // Started from the snapshot, the callback ran in the zygote. A plain Worker runs it once
      // a collection has found the target and the handler has yielded (cleanup tasks run
      // between runs of JavaScript).
      const ranInZygote = finalization.cleaned.length === 1;
      if (!ranInZygote) {
        collect();
        await scheduler.wait(1);
      }
      result.cleanupInZygote =
        !unsafe.startedFromStartupSnapshot() || ranInZygote;
      result.cleanupRan = finalization.cleaned.join() === 'b';
      result.headersFromCleanup =
        finalization.headers !== null &&
        finalization.headers.get('x-held') === 'b';
      break;
    }
    case 'bindings':
      // Capability bindings. The RPC calls go through the wildcard property handler of the
      // Fetcher template and of a subclass's.
      result.rpc = (await env.RPC.ping()) === 'pong';
      result.durableObject =
        (await env.NS.get(env.NS.idFromName('a')).ping()) === 'pong';
      break;
  }
  return result;
}

export async function report(extraChecks = {}) {
  return {
    mode: MODE,
    snapshotUsed: unsafe.startedFromStartupSnapshot(),
    envKeys: Object.keys(env),
    checks: { ...(await checks()), ...extraChecks },
  };
}

async function fetch() {
  return Response.json(await report());
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

async function test(ctrl, env) {
  // The runner went through the same pipeline as the cases: whether it was started from a
  // snapshot says whether the pipeline is on in this variant.
  const pipelineOn = unsafe.startedFromStartupSnapshot();

  // Cases whose top level the zygote can carry: started from the snapshot when the pipeline is
  // on.
  const transferable = [
    'control',
    'state',
    'state-url-standard',
    'gc',
    'finalization',
    'bindings',
  ];
  // Cases that fail the zygote, or do not qualify for the pipeline: always plain Workers. The
  // module scope of node:fs reaches UrlUtil, a Rust JSG resource (src/rust/api/url.rs); Rust
  // resource types have no snapshot transfer, so the fs case falls back until they do.
  const fallback = ['fs', 'unclonable', 'searchparams', 'no-ts-streams'];

  const run = async (name, expectedSnapshotUsed) => {
    const response = await env[name].fetch('http://case/');
    const r = await response.json();
    strictEqual(r.mode, name);
    strictEqual(r.snapshotUsed, expectedSnapshotUsed, `${name}: snapshotUsed`);
    for (const [check, value] of Object.entries(r.checks)) {
      strictEqual(value, true, `${name}: ${check}`);
    }
    return r;
  };
  for (const name of transferable) {
    const r = await run(name, pipelineOn);
    if (name === 'bindings') {
      // Every binding of the config is present, in the config's order.
      deepStrictEqual(
        r.envKeys,
        ['MODE', 'TEXT', 'RPC', 'JSON', 'NS', 'DATA'],
        'bindings: envKeys'
      );
    }
  }
  for (const name of fallback) {
    await run(name, false);
  }
}

// Only the runner has a test handler: workerd test runs test() of every service that exports one.
export default MODE === 'runner' ? { fetch, test } : { fetch };
