// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Live API objects across a startup snapshot. A case Worker's top level creates objects whose
// native side has to travel through the snapshot (Headers, URL, TextEncoder, crypto, navigator,
// the text/data/JSON bindings; node:fs in startup-snapshot-wrappables-fs-test.js, file system
// handles in startup-snapshot-wrappables-webfs-test.js) and at request time reports whether they
// still hold their state and still work. Negative cases leave an object the snapshot cannot carry
// reachable, or do not qualify for the pipeline, and must start as plain Workers. The runner
// asserts the reports. Under the startup-snapshot autogate every service here goes through the
// zygote; without it they are plain Workers and the same assertions hold with snapshotUsed false.

import { deepStrictEqual, strictEqual } from 'node:assert';
import {
  DurableObject,
  WorkerEntrypoint,
  env,
  tracing,
} from 'cloudflare:workers';
// Whether a Worker was started from a startup snapshot is asked at request time: at module scope
// the answer would be the zygote's and be baked into the snapshot.
import unsafe from 'workerd:unsafe';

const MODE = env.MODE;

// Top-level state every case creates. Each object is a JSG resource whose C++ side the snapshot
// carries either as a snapshotClone() (Headers, URL, TextDecoder) or by recreating a stateless
// type (TextEncoder, Crypto, SubtleCrypto, Navigator).
const headers = new Headers({ 'x-a': '1', 'content-type': 'text/plain' });
headers.append('x-a', '2');
const url = new URL('https://example.com/p/a/t/h?query=string&x=1#hash');
const encoder = new TextEncoder();
// An ICU decoder, and a windows-1252 one (a LegacyDecoder).
const decoder = new TextDecoder('utf-8', { fatal: true });
const legacyDecoder = new TextDecoder('windows-1252');
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
// The C++ streams cases: a promise the top level leaves pending, and the stream parked on it.
let gate = null;
let resolveGate = null;
let stream = null;
let pending = null;
// The performance cases.
let performanceAtTopLevel = null;
let nowAtTopLevel = 0;
// The span case.
let span = null;
let endedSpan = null;
// The finalization case's registry and what its cleanup callback did. The handler reads this
// object, which keeps the registry reachable: an unreachable registry never runs its callback.
const finalization = { registry: null, cleaned: [], headers: null };

switch (MODE) {
  case 'unclonable':
    // Neither stateless nor clonable: a signal that has aborted holds its reason, a JavaScript
    // value, so the zygote rejects the snapshot.
    controller = AbortSignal.abort('unclonable');
    break;
  case 'midstream-decoder':
    // A decoder with the first byte of a two-byte character pending.
    kept = new TextDecoder();
    kept.decode(new Uint8Array([0xc3]), { stream: true });
    break;
  case 'searchparams':
    // The C++ URLSearchParams is not transferable either. URL::snapshotClone() drops the one it
    // created lazily, but a reference the top level keeps makes it reachable. The TypeScript one
    // (the TS_URL autogate) is a JavaScript object over its URL's LegacyUrlHandle, and travels.
    params = url.searchParams;
    break;
  case 'cpp-streams-pending-pull':
    // A C++ stream whose pull() returns a promise the top level leaves pending: the controller
    // chains a C++ continuation onto it, whose opaque data no snapshot can carry. Not an async
    // pull(): a suspended async function fails the zygote earlier and for its own reason.
    gate = new Promise((resolve) => (resolveGate = resolve));
    stream = new ReadableStream({
      pull(c) {
        return gate.then((chunk) => {
          c.enqueue(chunk);
          c.close();
        });
      },
    });
    break;
  case 'cpp-streams-pending-cancel':
    // cancel() returns a promise the top level leaves pending: as with pull(), the C++ side chains
    // a continuation onto it.
    gate = new Promise((resolve) => (resolveGate = resolve));
    stream = new ReadableStream({
      cancel() {
        return gate;
      },
    });
    pending = stream.cancel();
    break;
  case 'performance':
    // Touching the global creates the Performance object, which the snapshot clones.
    performanceAtTopLevel = performance;
    nowAtTopLevel = performance.now();
    break;
  case 'performance-entries':
    // A recorded entry is a jsg::Ref the clone cannot carry.
    performance.mark('top-level');
    break;
  case 'span':
    // Outside a request tracing.startSpan() returns a span that records nothing, which the
    // snapshot clones. Nothing the top level does to such a span leaves state behind.
    span = tracing.startSpan('top-level');
    span.setAttribute('key', 'value');
    endedSpan = tracing.startSpan('ended');
    endedSpan.end();
    break;
  case 'abort':
    // An AbortController and its signal, both wrapped at the top level: the restored controller
    // must still own the restored signal.
    controller = new AbortController();
    kept = controller.signal;
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
    decoder:
      decoder.fatal && decoder.decode(encoder.encode('héllo')) === 'héllo',
    legacyDecoder:
      legacyDecoder.encoding === 'windows-1252' &&
      legacyDecoder.decode(new Uint8Array([0x80])) === '€',
    randomUUID: /^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$/.test(
      cryptoAtTopLevel.randomUUID()
    ),
    digest:
      hex ===
      'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
    userAgent: navigatorAtTopLevel.userAgent === 'Cloudflare-Workers',
    // The global scope's own objects are the ones the top level captured, as without a snapshot.
    cryptoIdentity: cryptoAtTopLevel === crypto,
    subtleIdentity: subtle === crypto.subtle,
    navigatorIdentity: navigatorAtTopLevel === navigator,
    textBinding: env.TEXT === 'hello' && env.TEXT === textAtTopLevel,
    jsonBinding: env.JSON === jsonAtTopLevel && env.JSON.answer === 42,
    dataBinding:
      env.DATA === dataAtTopLevel &&
      env.DATA instanceof ArrayBuffer &&
      new Uint8Array(env.DATA).join() === '97,98,99',
  };
  switch (MODE) {
    case 'unclonable':
      result.controller = controller.aborted;
      break;
    case 'searchparams':
      result.params = params.get('query') === 'string';
      // The URL and its searchParams still share one query.
      params.set('x', '2');
      result.paramsShared =
        params === url.searchParams && url.search === '?query=string&x=2';
      break;
    case 'midstream-decoder':
      result.midStreamDecoder = kept.decode(new Uint8Array([0xa9])) === 'é';
      break;
    case 'abort':
      result.abortSignalIdentity = controller.signal === kept;
      result.abortNotYet = !kept.aborted;
      controller.abort('now');
      result.abortWorks = kept.aborted && kept.reason === 'now';
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
    case 'cpp-streams-pending-pull':
      resolveGate(new TextEncoder().encode('ok'));
      result.pendingPullRead = (await new Response(stream).text()) === 'ok';
      break;
    case 'cpp-streams-pending-cancel':
      resolveGate();
      await pending;
      result.pendingCancelSettled = true;
      break;
    case 'performance':
      result.performanceIdentity = performanceAtTopLevel === performance;
      result.performanceNow = performance.now() >= nowAtTopLevel;
      result.performanceMark =
        performance.mark('request').name === 'request' &&
        performance.getEntriesByName('request').length === 1;
      break;
    case 'performance-entries':
      result.performanceEntries =
        performance.getEntriesByName('top-level').length === 1;
      break;
    case 'span': {
      const context = span.spanContext();
      const requestSpan = tracing.startSpan('request');
      result.spanType =
        span instanceof tracing.Span && endedSpan instanceof tracing.Span;
      result.spanDistinct = span !== endedSpan;
      result.spanNotTraced = !span.isTraced && !endedSpan.isTraced;
      result.spanContext =
        context.traceId === '0'.repeat(32) &&
        context.spanId === '0'.repeat(16) &&
        context.traceFlags === 0;
      // Every mutator returns the span it was called on.
      result.spanChaining =
        span.setAttribute('key', 'value') === span &&
        span.setAttributes({ a: 1, b: true }) === span &&
        span.updateName('renamed') === span &&
        span.setStatus({ code: 'error', message: 'failed' }) === span;
      span.recordException(new Error('boom'));
      span.end();
      endedSpan.end();
      result.spanEnded = !span.isTraced && !endedSpan.isTraced;
      // The runtime takes the span as an argument and hands the same object back.
      result.spanActive =
        tracing.withActiveSpan(span, () => tracing.getActiveSpan()) === span;
      // A span of this request is another object of the same type.
      result.spanOfRequest =
        requestSpan !== span &&
        Object.getPrototypeOf(requestSpan) === Object.getPrototypeOf(span);
      requestSpan.end();
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
  // Only the @all-autogates variant runs the pipeline, and TEST_WORKERD is on exactly there. The
  // runner went through the same pipeline as the cases, so it must have started from the
  // snapshot exactly when the pipeline is on; otherwise its top level is not transferable, and
  // every case would be checked against a pipeline that is off.
  const pipelineOn = unsafe.isTestAutogateEnabled();
  strictEqual(
    unsafe.startedFromStartupSnapshot(),
    pipelineOn,
    'runner: snapshotUsed'
  );
  // Whether URL is the TypeScript implementation (the TS_URL autogate), which decides whether the
  // searchparams case can be carried. User code cannot read an autogate, but unlike a JSG class,
  // a TypeScript class's source text is not [native code].
  const tsUrl = !Function.prototype.toString
    .call(URL)
    .includes('[native code]');

  // Cases whose top level the zygote can carry: started from the snapshot when the pipeline is
  // on.
  const transferable = [
    'control',
    'state',
    'state-url-standard',
    'abort',
    'gc',
    'finalization',
    'bindings',
    // C++ streams: the pipeline takes a Worker without typescript_implemented_streams all the
    // same; only a C++ stream with a parked pull() would fail its zygote, and this module scope
    // has none.
    'cpp-streams',
    'performance',
    'span',
    // File system handles: see startup-snapshot-wrappables-webfs-test.js.
    'webfs-tmp',
    'webfs-bundle',
    // node:fs and node:url: the module scope reaches UrlUtil, a Rust JSG resource
    // (src/rust/api/url.rs) marked snapshot_stateless.
    'fs',
    ...(tsUrl ? ['searchparams'] : []),
  ];
  // Cases that fail the zygote: always plain Workers.
  const fallback = [
    'unclonable',
    ...(tsUrl ? [] : ['searchparams']),
    'midstream-decoder',
    // C++ streams left awaiting a promise: a C++ continuation the snapshot cannot carry.
    'cpp-streams-pending-pull',
    'cpp-streams-pending-cancel',
    // A recorded performance entry is JavaScript state the clone cannot carry.
    'performance-entries',
  ];

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
