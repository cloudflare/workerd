// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Live API objects across a startup snapshot. A case Worker's top level creates objects whose
// native side has to travel through the snapshot (Headers, URL, TextEncoder, crypto, navigator,
// the text/data/JSON bindings; CryptoKeys in the crypto-keys case; node:fs in
// startup-snapshot-wrappables-fs-test.js, file system handles in
// startup-snapshot-wrappables-webfs-test.js) and at request time reports whether they still hold
// their state and still work. Negative cases leave an object the snapshot cannot carry
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

// Fixed keys for the crypto cases.
const EC_JWK = {
  kty: 'EC',
  crv: 'P-256',
  x: '_w8CT04oAPekH1gM6xiOVPX14wM7h5bj0W95tF0WL1s',
  y: 'zZ0H57YkMq_Taaa-oYyink-RlVw3t9xrabj4cfBC5ks',
  d: 'xHDBUG6nHIKnSYC0B2w_b54aLmaev81Slo7sW6Rgjj4',
};
const ED25519_JWK = {
  kty: 'OKP',
  crv: 'Ed25519',
  x: 'sTmjguLELrzU3VPBOcA_cguPH8CYso9jVrSgN9a2aqM',
  d: 'GzqCoxDqYdXR3nxkmYguU1Lr5UC52zByLW_kIoX_0po',
};
const RSA_N =
  'tmLA_YIxKDNFmz9bpIuoNRueaAKC8CjeWaYNH3HphuohsE2ycNARdFbD5QPfuGRkJxO77Dh6gRuU5zpZQqasBfoo-2Rh' +
  'dhV7hVuhWyhcnNF-PpnaOntrD_1eLXBPlEWkkbmqoRdTferBsR8H7F0tyQ6NpZB-BoZcPb0RYNdDSHwiiBMG1m9qnX2G' +
  'dh4KyJ7E34EHogygKa4nCdB79fu8sAZsj-xB3PvkyUKkezEfkAaDAVnpXrVDC56MD88PWjV8WHKeOy1nGQp9A4srS9_5' +
  'dR2nSp79FjufZiDyBK3E3S-f1Zva2JAcKNR68yDUERDWTmQQcDpOdanyAvGVRl5-9Q';

// Per-case top-level state.
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
    // Neither stateless nor clonable: the zygote rejects the snapshot.
    kept = new FormData();
    break;
  case 'event-targets': {
    // Plain EventTargets with listeners: a function, a once listener, a handler object, and an
    // instance of a JavaScript subclass. Each listener records into `kept.calls`.
    const target = new EventTarget();
    const onceListener = () => kept.calls.push('once');
    target.addEventListener('ping', (event) =>
      kept.calls.push(`ping:${event.type}`)
    );
    target.addEventListener('ping', onceListener, { once: true });
    class App extends EventTarget {}
    const app = new App();
    const handlerObject = {
      handleEvent(event) {
        kept.calls.push(`object:${this === handlerObject}:${event.type}`);
      },
    };
    app.addEventListener('pong', handlerObject);
    kept = {
      target,
      onceListener,
      app,
      App,
      handlerObject,
      empty: new EventTarget(),
      calls: [],
    };
    break;
  }
  case 'event-target-signal':
    // A listener registered with a `signal`: its removal is the signal's state.
    kept = new EventTarget();
    kept.addEventListener('ping', () => {}, {
      signal: new AbortController().signal,
    });
    break;
  case 'abort': {
    // A controller whose signal JavaScript holds, with a listener; one whose signal it never got;
    // a signal aborted with a primitive reason, and a controller aborted with an object one.
    const used = new AbortController();
    const signal = used.signal;
    signal.addEventListener('abort', () =>
      kept.aborts.push(`listener:${signal.reason}`)
    );
    const reasonObject = { why: 'object' };
    const abortedController = new AbortController();
    const abortedSignal = abortedController.signal;
    abortedController.abort(reasonObject);
    kept = {
      used,
      signal,
      unused: new AbortController(),
      aborted: AbortSignal.abort('top-level reason'),
      abortedController,
      abortedSignal,
      reasonObject,
      aborts: [],
    };
    break;
  }
  case 'urlpattern':
  case 'urlpattern-standard':
    // Patterns with a RegExp component, with only exact-match and empty components, and with
    // wildcards.
    kept = {
      books: new URLPattern({ pathname: '/books/:id(\\d+)' }),
      about: new URLPattern({ pathname: '/about' }),
      wild: new URLPattern('https://*.example.com/*'),
    };
    break;
  case 'caches-scheduler':
    // The global caches and scheduler, and the default cache, as the top level kept them.
    kept = { caches, defaultCache: caches.default, scheduler };
    break;
  case 'abort-unread-aborted':
    // Aborted before JavaScript got its signal: the signal has no wrapper of its own to carry
    // the abort.
    kept = new AbortController();
    kept.abort();
    break;
  case 'midstream-decoder':
    // A decoder with the first byte of a two-byte character pending.
    kept = new TextDecoder();
    kept.decode(new Uint8Array([0xc3]), { stream: true });
    break;
  case 'streamed-decoder': {
    // A UTF-8 decoder mid-stream between characters, past a BOM it stripped. What it carries is
    // that it has seen the BOM, so a later one is kept.
    const streamed = new TextDecoder();
    const bytes = new Uint8Array([0xef, 0xbb, 0xbf, 0xc3, 0xa9]);
    kept = {
      decoder: streamed,
      first: streamed.decode(bytes, { stream: true }),
    };
    break;
  }
  case 'crypto-keys': {
    // Keys whose Impl copies itself: secret key material, and asymmetric keys sharing their
    // EVP_PKEY. None of their `algorithm` or `usages` properties is read here. The top level may
    // not generate random values, so every key is imported.
    const importPair = async (jwk, algorithm) => ({
      privateKey: await subtle.importKey('jwk', jwk, algorithm, false, [
        'sign',
      ]),
      publicKey: await subtle.importKey(
        'jwk',
        { ...jwk, d: undefined },
        algorithm,
        false,
        ['verify']
      ),
    });
    kept = {
      hmac: await subtle.importKey(
        'raw',
        encoder.encode('0123456789abcdef'),
        { name: 'HMAC', hash: 'SHA-256' },
        false,
        ['sign', 'verify']
      ),
      aes: await subtle.importKey(
        'raw',
        new Uint8Array(32).fill(7),
        'AES-GCM',
        true,
        ['encrypt', 'decrypt']
      ),
      ecdsa: await importPair(EC_JWK, { name: 'ECDSA', namedCurve: 'P-256' }),
      ed25519: await importPair(ED25519_JWK, { name: 'Ed25519' }),
      pbkdf2: await subtle.importKey(
        'raw',
        encoder.encode('password'),
        'PBKDF2',
        false,
        ['deriveBits']
      ),
    };
    break;
  }
  case 'crypto-key-algorithm':
    // A key whose `algorithm` object JavaScript has seen: its identity cannot be carried.
    kept = await subtle.importKey(
      'raw',
      encoder.encode('0123456789abcdef'),
      { name: 'HMAC', hash: 'SHA-256' },
      false,
      ['sign']
    );
    kept.algorithm.seen = true;
    break;
  case 'crypto-key-rsa':
    // An RSA key's algorithm holds its public exponent in a V8 buffer, which a copy cannot.
    kept = await subtle.importKey(
      'jwk',
      { kty: 'RSA', n: RSA_N, e: 'AQAB' },
      { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' },
      false,
      ['verify']
    );
    break;
  case 'searchparams':
  case 'searchparams-url-standard':
    // A URL's searchParams the top level keeps, which must still share the restored URL's query,
    // and a standalone one with a repeated name. The TypeScript URL (the TS_URL autogate) is a
    // JavaScript object over its URL's LegacyUrlHandle.
    params = url.searchParams;
    kept = new URLSearchParams('a=1&b=2&a=3&c=x%20y+z');
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
      result.formData = kept instanceof FormData;
      break;
    case 'event-targets': {
      const { target, onceListener, app, App, handlerObject, empty } = kept;
      target.dispatchEvent(new Event('ping'));
      target.dispatchEvent(new Event('ping'));
      // The identity the top level registered still removes the listener (a no-op once fired).
      target.removeEventListener('ping', onceListener);
      app.dispatchEvent(new Event('pong'));
      app.removeEventListener('pong', handlerObject);
      app.dispatchEvent(new Event('pong'));
      result.eventTargetCalls =
        kept.calls.join() === 'ping:ping,once,ping:ping,object:true:pong';
      result.eventTargetSubclass =
        app instanceof App &&
        app instanceof EventTarget &&
        app.constructor.name === 'App';
      result.eventTargetEmpty = empty.dispatchEvent(new Event('x'));
      break;
    }
    case 'event-target-signal':
      result.eventTarget = kept instanceof EventTarget;
      break;
    case 'abort': {
      const {
        used,
        signal,
        unused,
        aborted,
        abortedController,
        abortedSignal,
        reasonObject,
      } = kept;
      result.abortSignalIdentity = used.signal === signal && !signal.aborted;
      used.abort('request reason');
      result.abortListener =
        signal.aborted && kept.aborts.join() === 'listener:request reason';
      result.abortUnused = !unused.signal.aborted;
      unused.abort();
      result.abortUnusedAborts = unused.signal.aborted;
      result.abortStatic =
        aborted.aborted && aborted.reason === 'top-level reason';
      result.abortObjectReason =
        abortedController.signal === abortedSignal &&
        abortedSignal.reason === reasonObject;
      try {
        abortedSignal.throwIfAborted();
        result.abortThrows = false;
      } catch (e) {
        result.abortThrows = e === reasonObject;
      }
      break;
    }
    case 'urlpattern':
    case 'urlpattern-standard': {
      const { books, about, wild } = kept;
      result.urlPatternMatch =
        books.test('https://x/books/42') &&
        books.exec('https://x/books/42').pathname.groups.id === '42' &&
        !books.test('https://x/books/abc') &&
        books.pathname === '/books/:id(\\d+)';
      result.urlPatternExact =
        about.test('https://x/about') && !about.test('https://x/other');
      result.urlPatternWildcard =
        wild.test('https://a.example.com/q') &&
        !wild.test('https://example.org/q') &&
        wild.hostname === '*.example.com';
      break;
    }
    case 'caches-scheduler':
      result.cachesIdentity =
        kept.caches === caches && kept.scheduler === scheduler;
      result.defaultCacheIdentity = kept.defaultCache === caches.default;
      result.scheduler = typeof kept.scheduler.wait === 'function';
      break;
    case 'abort-unread-aborted':
      result.abortUnread = kept.signal.aborted;
      break;
    case 'searchparams':
    case 'searchparams-url-standard':
      result.standaloneParams =
        kept.getAll('a').join() === '1,3' &&
        kept.get('c') === 'x y z' &&
        kept.toString() === 'a=1&b=2&a=3&c=x+y+z';
      kept.append('d', '4');
      result.standaloneParamsMutable = kept.get('d') === '4';
      result.params = params.get('query') === 'string';
      // The URL and its searchParams still share one query.
      params.set('x', '2');
      result.paramsShared =
        params === url.searchParams && url.search === '?query=string&x=2';
      break;
    case 'midstream-decoder':
      result.midStreamDecoder = kept.decode(new Uint8Array([0xa9])) === 'é';
      break;
    case 'streamed-decoder':
      result.streamedDecoder =
        kept.first === 'é' &&
        kept.decoder.decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x61])) ===
          '\ufeffa';
      break;
    case 'crypto-keys':
      Object.assign(result, await cryptoKeyChecks(kept));
      break;
    case 'crypto-key-algorithm':
      result.algorithmIdentity = kept.algorithm.seen === true;
      break;
    case 'crypto-key-rsa':
      result.rsa = kept.type === 'public';
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

// The crypto-keys case: each key still works, with its options.
async function cryptoKeyChecks({ hmac, aes, ecdsa, ed25519, pbkdf2 }) {
  const data = encoder.encode('abc');
  const toHex = (buffer) =>
    [...new Uint8Array(buffer)]
      .map((b) => b.toString(16).padStart(2, '0'))
      .join('');
  const hmacSignature = await subtle.sign('HMAC', hmac, data);
  const iv = new Uint8Array(12);
  const cipherText = await subtle.encrypt({ name: 'AES-GCM', iv }, aes, data);
  const plainText = await subtle.decrypt(
    { name: 'AES-GCM', iv },
    aes,
    cipherText
  );
  const exported = await subtle.exportKey('raw', aes);
  const ecdsaAlgorithm = { name: 'ECDSA', hash: 'SHA-256' };
  const ecdsaSignature = await subtle.sign(
    ecdsaAlgorithm,
    ecdsa.privateKey,
    data
  );
  const edSignature = await subtle.sign('Ed25519', ed25519.privateKey, data);
  const bits = await subtle.deriveBits(
    { name: 'PBKDF2', hash: 'SHA-256', salt: data, iterations: 1 },
    pbkdf2,
    128
  );
  return {
    hmac:
      toHex(hmacSignature) ===
        toHex(
          await subtle.sign(
            'HMAC',
            await subtle.importKey(
              'raw',
              encoder.encode('0123456789abcdef'),
              { name: 'HMAC', hash: 'SHA-256' },
              false,
              ['sign']
            ),
            data
          )
        ) && (await subtle.verify('HMAC', hmac, hmacSignature, data)),
    hmacOptions:
      !hmac.extractable &&
      hmac.usages.join() === 'sign,verify' &&
      hmac.algorithm.hash.name === 'SHA-256',
    aes:
      new TextDecoder().decode(plainText) === 'abc' &&
      aes.extractable &&
      exported.byteLength === 32 &&
      aes.algorithm.length === 256,
    ecdsa:
      (await subtle.verify(
        ecdsaAlgorithm,
        ecdsa.publicKey,
        ecdsaSignature,
        data
      )) &&
      ecdsa.privateKey.type === 'private' &&
      ecdsa.publicKey.algorithm.namedCurve === 'P-256',
    ed25519: await subtle.verify(
      'Ed25519',
      ed25519.publicKey,
      edSignature,
      data
    ),
    pbkdf2: bits.byteLength === 16,
  };
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
    'streamed-decoder',
    'crypto-keys',
    'event-targets',
    'searchparams',
    'searchparams-url-standard',
    'caches-scheduler',
    'urlpattern',
    'urlpattern-standard',
  ];
  // Cases that fail the zygote: always plain Workers.
  const fallback = [
    'unclonable',
    'midstream-decoder',
    // C++ streams left awaiting a promise: a C++ continuation the snapshot cannot carry.
    'cpp-streams-pending-pull',
    'cpp-streams-pending-cancel',
    // A recorded performance entry is JavaScript state the clone cannot carry.
    'performance-entries',
    'crypto-key-algorithm',
    'crypto-key-rsa',
    'event-target-signal',
    'abort-unread-aborted',
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
