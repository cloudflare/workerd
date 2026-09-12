// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tearing the exchange down: destroying the request aborts the fetch (or
// cancels the response body) and reports it as Node does; destroying the
// response cancels the body stream underneath, which reaches the server;
// the server going away mid-body aborts the response; a failed connection
// errors the request; and the request closes once the exchange is over.

import { strictEqual, deepStrictEqual, ok, throws } from 'node:assert';
import { finished } from 'node:stream';
import {
  request,
  get,
  getRaw,
  response,
  collect,
  once,
  record,
  neverEndsStats,
  uniqueId,
} from 'harness';

// res.destroy() on a fresh response: 'close' follows, destroyed is set.
export const responseDestroyCloses = {
  async test(ctrl, env) {
    const res = await response(get(env, '/asd'));
    strictEqual(res.destroyed, false);
    res.destroy();
    strictEqual(res.destroyed, true);
    await once(res, 'close');
  },
};

// A consumed response ends, then closes — once — destroyed.
export const consumedResponseEndsThenCloses = {
  async test(ctrl, env) {
    const res = await response(get(env, '/asd'));
    const events = [];
    res.on('end', () => events.push(`end:${res.destroyed}`));
    res.on('close', () => events.push(`close:${res.destroyed}`));
    res.resume();
    await once(res, 'close');
    await scheduler.wait(10);
    deepStrictEqual(events, ['end:false', 'close:true']);
  },
};

// res.destroy(err) mid-body: 'aborted', the error on the response (and
// forwarded to the request), 'close'; the body stream is cancelled, which
// the server observes as its connection closing.
export const responseDestroyMidBodyReachesServer = {
  async test(ctrl, env) {
    const id = uniqueId('res-destroy');
    const log = [];
    const req = request(env, `/never-ends?id=${id}`);
    record(log, 'req', req, ['error']);
    req.end();
    const res = await response(req);
    record(log, 'res', res, ['aborted', 'error', 'end', 'close']);
    res.on('data', (chunk) => {
      log.push(`data:${chunk}`);
      res.destroy(new Error('enough'));
    });
    await once(res, 'close');
    deepStrictEqual(log, [
      'data:first',
      'res:aborted',
      'req:error(Error/-/enough)',
      'res:error(Error/-/enough)',
      'res:close',
    ]);
    strictEqual(res.aborted, true);
    strictEqual(res.complete, false);
    await scheduler.wait(50);
    deepStrictEqual(await neverEndsStats(env, id), {
      opened: true,
      closed: true,
    });
  },
};

// The server dropping the connection mid-body aborts the response: the
// body read fails, so 'aborted', the same error on response and request,
// then 'close'; complete stays false.
export const serverDroppingConnectionAbortsResponse = {
  async test(ctrl, env) {
    const log = [];
    const errors = [];
    const req = request(env, '/error-mid-body');
    req.on('error', (err) => errors.push(err));
    req.end();
    const res = await response(req);
    res.on('error', (err) => errors.push(err));
    record(log, 'res', res, ['aborted', 'end', 'close']);
    res.on('data', (chunk) => log.push(`data:${chunk}`));
    await once(res, 'close');
    deepStrictEqual(log, ['data:partial', 'res:aborted', 'res:close']);
    strictEqual(errors.length, 2);
    strictEqual(errors[0], errors[1]);
    ok(errors[0] instanceof Error);
    strictEqual(res.aborted, true);
    strictEqual(res.complete, false);
  },
};

// A response read to the end is complete and not aborted; a second
// consumer sees nothing more.
export const completedResponseIsFinal = {
  async test(ctrl, env) {
    const res = await response(get(env, '/pong'));
    strictEqual((await collect(res)).toString(), 'pong');
    strictEqual(res.complete, true);
    strictEqual(res.aborted, false);
    strictEqual(res.readableEnded, true);
    strictEqual(res.read(), null);
  },
};

// A connection that cannot be made fails the request: 'error', then
// 'close'; no 'response'.
export const connectionFailureErrorsRequest = {
  async test(ctrl, env) {
    const log = [];
    const req = request(env, '/', { port: 1 });
    record(log, 'req', req, ['response', 'close']);
    const errors = [];
    req.on('error', (err) => {
      errors.push(err);
      log.push('req:error');
    });
    req.end();
    await once(req, 'close');
    deepStrictEqual(log, ['req:error', 'req:close']);
    ok(errors[0] instanceof Error);
    strictEqual(req.destroyed, true);
  },
};

// A body shorter than its Content-Length, then the connection closing
// (the raw server): the response arrives with the announced length, the
// bytes that came are delivered, then 'aborted', the same codeless error
// on the response and the request, and the closes; complete stays false.
export const truncatedBodyAbortsResponse = {
  async test(ctrl, env) {
    const log = [];
    const errors = [];
    const req = getRaw(env, '/short-body');
    req.on('error', (err) => {
      errors.push(err);
      log.push('req:error');
    });
    record(log, 'req', req, ['close']);
    const res = await response(req);
    strictEqual(res.statusCode, 200);
    strictEqual(res.headers['content-length'], '10');
    res.on('error', (err) => {
      errors.push(err);
      log.push('res:error');
    });
    record(log, 'res', res, ['aborted', 'end', 'close']);
    res.on('data', (chunk) => log.push(`data:${chunk}`));
    await once(res, 'close');
    deepStrictEqual(log, [
      'data:short',
      'res:aborted',
      'req:error',
      'res:error',
      'req:close',
      'res:close',
    ]);
    strictEqual(errors.length, 2);
    strictEqual(errors[0], errors[1]);
    ok(errors[0] instanceof Error);
    strictEqual(errors[0].code, undefined);
    strictEqual(res.complete, false);
  },
};

// Bytes beyond the Content-Length are not the body: the response ends,
// complete, after exactly the announced length.
export const bytesBeyondContentLengthAreIgnored = {
  async test(ctrl, env) {
    const res = await response(getRaw(env, '/long-body'));
    strictEqual(res.headers['content-length'], '4');
    strictEqual((await collect(res)).toString(), 'long');
    strictEqual(res.complete, true);
  },
};

// Malformed chunked framing after a good chunk: the good chunk is
// delivered, then the response aborts as a truncated one does, with the
// runtime's framing error on both response and request.
export const malformedChunkedFramingAbortsResponse = {
  async test(ctrl, env) {
    const log = [];
    const errors = [];
    const req = getRaw(env, '/bad-chunked');
    req.on('error', (err) => {
      errors.push(err);
      log.push('req:error');
    });
    record(log, 'req', req, ['close']);
    const res = await response(req);
    strictEqual(res.headers['transfer-encoding'], 'chunked');
    res.on('error', (err) => {
      errors.push(err);
      log.push('res:error');
    });
    record(log, 'res', res, ['aborted', 'end', 'close']);
    res.on('data', (chunk) => log.push(`data:${chunk}`));
    await once(res, 'close');
    deepStrictEqual(log, [
      'data:ok',
      'res:aborted',
      'req:error',
      'res:error',
      'req:close',
      'res:close',
    ]);
    strictEqual(errors.length, 2);
    strictEqual(errors[0], errors[1]);
    ok(errors[0] instanceof Error);
    strictEqual(res.complete, false);
  },
};

// Replies the runtime cannot parse at all never become a response: an
// empty reply (the connection closing without a byte) and a garbage status
// line each fail the request — 'error' with a codeless Error whose text is
// the runtime's, then 'close' — with no 'response'.
export const unparseableRepliesFailTheRequest = {
  async test(ctrl, env) {
    for (const path of ['/empty-reply', '/garbage']) {
      const log = [];
      const errors = [];
      const req = getRaw(env, path);
      record(log, 'req', req, ['response', 'close']);
      req.on('error', (err) => {
        errors.push(err);
        log.push('req:error');
      });
      await once(req, 'close');
      deepStrictEqual(log, ['req:error', 'req:close'], path);
      ok(errors[0] instanceof Error, path);
      strictEqual(errors[0].code, undefined, path);
      strictEqual(req.destroyed, true, path);
    }
  },
};

// The `signal` option. An already-aborted signal, or one aborted before
// end(): 'error' with an AbortError (ABORT_ERR) then 'close', nothing
// sent, no 'response'.
export const signalAbortBeforeSendFailsRequest = {
  async test(ctrl, env) {
    const already = new AbortController();
    already.abort();
    for (const [label, signal, abortNow] of [
      ['already aborted', already.signal, () => {}],
      ['aborted before end()', undefined, undefined],
    ]) {
      const controller = new AbortController();
      const log = [];
      const errors = [];
      const req = request(env, '/sink', {
        method: 'POST',
        signal: signal ?? controller.signal,
      });
      record(log, 'req', req, ['response', 'finish', 'close']);
      req.on('error', (err) => {
        errors.push(err);
        log.push('req:error');
      });
      req.write('never sent');
      (abortNow ?? (() => controller.abort()))();
      req.end();
      await once(req, 'close');
      deepStrictEqual(log, ['req:error', 'req:close'], label);
      strictEqual(errors[0].name, 'AbortError', label);
      strictEqual(errors[0].code, 'ABORT_ERR', label);
      strictEqual(req.destroyed, true, label);
    }
  },
};

// The signal stays armed for the whole exchange: aborted while the
// response body is arriving, the response aborts and both report an
// AbortError whose cause is the signal's reason — the shape of a timeout.
export const signalAbortMidBodyAbortsResponse = {
  async test(ctrl, env) {
    const controller = new AbortController();
    const log = [];
    const errors = [];
    const req = get(env, '/chunked?n=6&delay=30', {
      signal: controller.signal,
    });
    req.on('error', (err) => {
      errors.push(err);
      log.push('req:error');
    });
    record(log, 'req', req, ['close']);
    const res = await response(req);
    res.on('error', (err) => {
      errors.push(err);
      log.push('res:error');
    });
    record(log, 'res', res, ['aborted', 'end', 'close']);
    res.on('data', () => log.push('data'));
    await once(res, 'data');
    const reason = new Error('enough');
    controller.abort(reason);
    await once(res, 'close');
    deepStrictEqual(log, [
      'data',
      'res:aborted',
      'req:error',
      'req:close',
      'res:error',
      'res:close',
    ]);
    strictEqual(errors.length, 2);
    strictEqual(errors[0], errors[1]);
    strictEqual(errors[0].name, 'AbortError');
    strictEqual(errors[0].code, 'ABORT_ERR');
    strictEqual(errors[0].cause, reason);
    strictEqual(res.complete, false);
    strictEqual(req.destroyed, true);
  },
};

// Aborting the signal once the exchange is over changes nothing.
export const signalAbortAfterCompletionIsInert = {
  async test(ctrl, env) {
    const controller = new AbortController();
    const errors = [];
    const req = get(env, '/pong', { signal: controller.signal });
    req.on('error', (err) => errors.push(err));
    const res = await response(req);
    strictEqual((await collect(res)).toString(), 'pong');
    await once(req, 'close');
    controller.abort();
    await scheduler.wait(20);
    strictEqual(errors.length, 0);
    strictEqual(req.errored, null);
  },
};

// stream.finished(req) reports the end of the exchange (the request's
// 'close'), not the request having been sent ('finish'): as in Node,
// where the request is a legacy stream that finished() also waits on as a
// readable.
export const finishedOnRequestWaitsForClose = {
  async test(ctrl, env) {
    const req = get(env, '/chunked?n=2&delay=20');
    const events = [];
    req.on('finish', () => events.push('finish'));
    req.on('close', () => events.push('close'));
    const done = new Promise((resolve) =>
      finished(req, (err) => {
        events.push(`finished(${err === undefined ? '' : err.message})`);
        resolve();
      })
    );
    const res = await response(req);
    strictEqual((await collect(res)).toString(), 'chunk-0|chunk-1|');
    await done;
    deepStrictEqual(events, ['finish', 'close', 'finished()']);
  },
};

// The response is the request's `res`; a completed exchange closes the
// request after the response ends, and leaves it destroyed.
export const responseEndClosesRequest = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/asd');
    record(log, 'req', req, ['finish', 'error', 'close']);
    strictEqual(req.res, undefined);
    const res = await response(req);
    strictEqual(req.res, res);
    strictEqual(req.destroyed, false);
    record(log, 'res', res, ['end', 'close']);
    const closed = Promise.all([once(req, 'close'), once(res, 'close')]);
    res.resume();
    await closed;
    deepStrictEqual(log, ['req:finish', 'res:end', 'req:close', 'res:close']);
    strictEqual(req.destroyed, true);
  },
};

// A bare destroy() before any response: the request reports the
// connection as hung up (ECONNRESET), then closes; no 'response' follows,
// and the request never finishes.
export const destroyBeforeResponseHangsUp = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/slow-headers?delay=150');
    record(log, 'req', req, ['response', 'error', 'close']);
    await scheduler.wait(10);
    req.destroy();
    strictEqual(req.destroyed, true);
    await once(req, 'close');
    deepStrictEqual(log, [
      'req:error(Error/ECONNRESET/socket hang up)',
      'req:close',
    ]);
    await scheduler.wait(200);
    strictEqual(log.length, 2);
  },
};

// destroy(err) before any response reports that error instead.
export const destroyWithErrorBeforeResponse = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/slow-headers?delay=150');
    record(log, 'req', req, ['response', 'error', 'close']);
    await scheduler.wait(10);
    req.destroy(new Error('boom'));
    await once(req, 'close');
    deepStrictEqual(log, ['req:error(Error/-/boom)', 'req:close']);
    strictEqual(req.errored?.message, 'boom');
  },
};

// destroy() before end(): nothing is sent, end() is inert (no 'finish'),
// and the request hangs up and closes.
export const destroyBeforeEndSendsNothing = {
  async test(ctrl, env) {
    const log = [];
    const req = request(env, '/pong', { method: 'POST' });
    record(log, 'req', req, ['finish', 'response', 'error', 'close']);
    req.write('never sent');
    req.destroy();
    req.end();
    await once(req, 'close');
    await scheduler.wait(50);
    deepStrictEqual(log, [
      'req:error(Error/ECONNRESET/socket hang up)',
      'req:close',
    ]);
  },
};

// req.destroy() mid-body: the response is aborted at once and errors with
// ECONNRESET 'aborted' (for listeners), the request closes without an error
// of its own, the response closes; the body stream's cancellation reaches
// the server.
export const destroyMidBodyAbortsResponse = {
  async test(ctrl, env) {
    const id = uniqueId('req-destroy');
    const log = [];
    const req = get(env, `/never-ends?id=${id}`);
    record(log, 'req', req, ['error', 'close']);
    const res = await response(req);
    record(log, 'res', res, ['aborted', 'error', 'end', 'close']);
    const closed = Promise.all([once(req, 'close'), once(res, 'close')]);
    res.on('data', (chunk) => {
      log.push(`data:${chunk}`);
      req.destroy();
    });
    await closed;
    deepStrictEqual(log, [
      'data:first',
      'res:aborted',
      'req:close',
      'res:error(Error/ECONNRESET/aborted)',
      'res:close',
    ]);
    strictEqual(res.aborted, true);
    strictEqual(res.complete, false);
    await scheduler.wait(50);
    deepStrictEqual(await neverEndsStats(env, id), {
      opened: true,
      closed: true,
    });
  },
};

// req.destroy(err) mid-body: the response is aborted at once; the request
// reports err on the next tick and closes; the response errors with err
// and closes.
export const destroyWithErrorMidBody = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, `/never-ends?id=${uniqueId('req-destroy-err')}`);
    record(log, 'req', req, ['error', 'close']);
    const res = await response(req);
    record(log, 'res', res, ['aborted', 'error', 'close']);
    const closed = Promise.all([once(req, 'close'), once(res, 'close')]);
    res.on('data', () => req.destroy(new Error('enough')));
    await closed;
    deepStrictEqual(log, [
      'res:aborted',
      'req:error(Error/-/enough)',
      'req:close',
      'res:error(Error/-/enough)',
      'res:close',
    ]);
  },
};

// A response that arrives after the request was destroyed is dropped, its
// body cancelled: no 'response' ever fires.
export const responseAfterDestroyIsDropped = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/asd');
    record(log, 'req', req, ['response', 'error', 'close']);
    req.destroy();
    await once(req, 'close');
    await scheduler.wait(100);
    deepStrictEqual(log, [
      'req:error(Error/ECONNRESET/socket hang up)',
      'req:close',
    ]);
  },
};

// abort() before any response is the quiet teardown: aborted and destroyed
// are set at once, 'abort' then 'close' fire, no 'error'; a second abort()
// is inert.
export const abortBeforeResponseIsQuiet = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/slow-headers?delay=150');
    record(log, 'req', req, ['abort', 'response', 'error', 'close']);
    await scheduler.wait(10);
    strictEqual(req.aborted, false);
    req.abort();
    strictEqual(req.aborted, true);
    strictEqual(req.destroyed, true);
    req.abort();
    await once(req, 'close');
    await scheduler.wait(200);
    deepStrictEqual(log, ['req:abort', 'req:close']);
  },
};

// abort() before end(): nothing is sent, end() is inert.
export const abortBeforeEndSendsNothing = {
  async test(ctrl, env) {
    const log = [];
    const req = request(env, '/pong', { method: 'POST' });
    record(log, 'req', req, ['abort', 'finish', 'response', 'error', 'close']);
    req.write('never sent');
    req.abort();
    req.end();
    await once(req, 'close');
    await scheduler.wait(50);
    deepStrictEqual(log, ['req:abort', 'req:close']);
  },
};

// abort() mid-body: the response is aborted at once and errors with
// ECONNRESET 'aborted' (for listeners); 'abort' and 'close' fire on the
// request, no 'error'; the server sees the connection close.
export const abortMidBodyAbortsResponse = {
  async test(ctrl, env) {
    const id = uniqueId('req-abort');
    const log = [];
    const req = get(env, `/never-ends?id=${id}`);
    record(log, 'req', req, ['abort', 'error', 'close']);
    const res = await response(req);
    record(log, 'res', res, ['aborted', 'error', 'end', 'close']);
    const closed = Promise.all([once(req, 'close'), once(res, 'close')]);
    res.on('data', (chunk) => {
      log.push(`data:${chunk}`);
      req.abort();
    });
    await closed;
    deepStrictEqual(log, [
      'data:first',
      'res:aborted',
      'req:abort',
      'req:close',
      'res:error(Error/ECONNRESET/aborted)',
      'res:close',
    ]);
    strictEqual(res.aborted, true);
    strictEqual(res.complete, false);
    await scheduler.wait(50);
    deepStrictEqual(await neverEndsStats(env, id), {
      opened: true,
      closed: true,
    });
  },
};

// The timeout fires once, whether armed before or after end(), and tears
// the request down with an AbortError: 'timeout', 'error', 'close'; no
// 'response'.
export const timeoutBeforeHeadersDestroysRequest = {
  async test(ctrl, env) {
    for (const armBeforeEnd of [true, false]) {
      const log = [];
      const req = request(env, '/slow-headers?delay=300');
      record(log, 'req', req, ['timeout', 'response', 'error', 'close']);
      if (armBeforeEnd) req.setTimeout(50);
      req.end();
      if (!armBeforeEnd) req.setTimeout(50);
      await once(req, 'close');
      await scheduler.wait(350);
      deepStrictEqual(log, [
        'req:timeout',
        'req:error(AbortError/ABORT_ERR/The operation was aborted)',
        'req:close',
      ]);
      strictEqual(req.destroyed, true);
    }
  },
};

// The `timeout` option arms the same timer, and the setTimeout() callback
// is the 'timeout' listener.
export const timeoutOptionAndCallback = {
  async test(ctrl, env) {
    const log = [];
    const req = request(env, '/slow-headers?delay=300', { timeout: 50 });
    req.on('error', () => log.push('error'));
    req.setTimeout(40, () => log.push('callback'));
    req.end();
    await once(req, 'close');
    deepStrictEqual(log, ['callback', 'error']);
  },
};

// A timeout mid-body: 'timeout' on the request and the response, then the
// response is aborted and both error with the AbortError and close.
export const timeoutMidBodyAbortsResponse = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/slow-body?delay=300');
    req.setTimeout(80);
    record(log, 'req', req, ['timeout', 'error', 'close']);
    const res = await response(req);
    record(log, 'res', res, ['timeout', 'aborted', 'error', 'end', 'close']);
    res.on('data', (chunk) => log.push(`data:${chunk}`));
    await Promise.all([once(req, 'close'), once(res, 'close')]);
    deepStrictEqual(log, [
      'data:first',
      'req:timeout',
      'res:timeout',
      'res:aborted',
      'req:error(AbortError/ABORT_ERR/The operation was aborted)',
      'req:close',
      'res:error(AbortError/ABORT_ERR/The operation was aborted)',
      'res:close',
    ]);
    strictEqual(res.complete, false);
  },
};

// A completed exchange disarms the timer: no 'timeout' afterwards.
export const timeoutDisarmedByCompletion = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/asd');
    req.setTimeout(40);
    record(log, 'req', req, ['timeout', 'error']);
    const res = await response(req);
    strictEqual((await collect(res)).toString(), 'asd');
    await scheduler.wait(100);
    deepStrictEqual(log, []);
  },
};

// A response arriving with no 'response' listener is dumped, as Node's is
// ("the response will be entirely discarded"): its body is consumed and
// dropped, the response completes and the request closes, so finished(req)
// resolves and nothing holds the exchange open. req.res is set all the
// same.
export const unhandledResponseIsDumped = {
  async test(ctrl, env) {
    const req = get(env, '/asd');
    const events = [];
    req.on('finish', () => events.push('finish'));
    req.on('close', () => events.push('close'));
    finished(req, (err) => {
      events.push(`finished(${err === undefined ? '' : err.message})`);
    });
    const outcome = await Promise.race([
      once(req, 'close').then(() => 'closed'),
      scheduler.wait(1000).then(() => 'still open'),
    ]);
    if (outcome !== 'closed') req.destroy();
    strictEqual(outcome, 'closed');
    strictEqual(req.res.complete, true);
    deepStrictEqual(events, ['finish', 'close', 'finished()']);
  },
};

// setTimeout(0) clears a pending timeout; the response then arrives.
export const setTimeoutZeroClears = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/slow-headers?delay=60');
    req.setTimeout(20);
    req.setTimeout(0);
    record(log, 'req', req, ['timeout', 'error']);
    const res = await response(req);
    strictEqual((await collect(res)).toString(), 'late');
    deepStrictEqual(log, []);
  },
};

// The response's setTimeout() sets the exchange's one timer — in Node the
// socket's idle timer, which the request's setTimeout() sets too — so a
// body that goes quiet fires 'timeout' on both sides and tears the
// exchange down as a request timeout does. The callback is the response's
// 'timeout' listener; setTimeout() returns the response.
export const responseSetTimeoutArmsTheTimer = {
  async test(ctrl, env) {
    const id = uniqueId('res-timeout');
    const log = [];
    const req = get(env, `/never-ends?id=${id}`);
    record(log, 'req', req, ['timeout', 'error', 'close']);
    const res = await response(req);
    record(log, 'res', res, ['timeout', 'aborted', 'error', 'end', 'close']);
    res.on('data', (chunk) => log.push(`data:${chunk}`));
    strictEqual(
      res.setTimeout(50, () => log.push('res:callback')),
      res
    );
    await Promise.all([once(req, 'close'), once(res, 'close')]);
    deepStrictEqual(log, [
      'data:first',
      'req:timeout',
      'res:timeout',
      'res:callback',
      'res:aborted',
      'req:error(AbortError/ABORT_ERR/The operation was aborted)',
      'req:close',
      'res:error(AbortError/ABORT_ERR/The operation was aborted)',
      'res:close',
    ]);
    strictEqual(res.complete, false);
  },
};

// One timer, whichever side set it last: a response's shorter timeout
// replaces the request's longer one.
export const responseSetTimeoutReplacesRequestTimeout = {
  async test(ctrl, env) {
    const id = uniqueId('res-timeout-shorter');
    const log = [];
    const req = get(env, `/never-ends?id=${id}`);
    req.setTimeout(30_000);
    record(log, 'req', req, ['timeout', 'error', 'close']);
    const res = await response(req);
    res.resume();
    const started = Date.now();
    res.setTimeout(50);
    await once(req, 'close');
    ok(Date.now() - started < 10_000, 'the response timeout fired');
    deepStrictEqual(log, [
      'req:timeout',
      'req:error(AbortError/ABORT_ERR/The operation was aborted)',
      'req:close',
    ]);
  },
};

// setTimeout(0) from the response clears the request's pending timeout.
export const responseSetTimeoutZeroClears = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/slow-body?delay=100');
    req.setTimeout(40);
    record(log, 'req', req, ['timeout', 'error']);
    const res = await response(req);
    res.setTimeout(0);
    strictEqual((await collect(res)).toString(), 'firstlast');
    deepStrictEqual(log, []);
  },
};

// msecs is validated on either side as Node's socket.setTimeout validates
// it: a non-number is ERR_INVALID_ARG_TYPE, a negative or non-finite one
// ERR_OUT_OF_RANGE; nothing is armed by a rejected call.
export const setTimeoutValidatesMsecs = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/slow-body?delay=60');
    record(log, 'req', req, ['timeout', 'error']);
    const res = await response(req);
    for (const target of [req, res]) {
      throws(() => target.setTimeout('abc'), { code: 'ERR_INVALID_ARG_TYPE' });
      throws(() => target.setTimeout(-1), { code: 'ERR_OUT_OF_RANGE' });
      throws(() => target.setTimeout(NaN), { code: 'ERR_OUT_OF_RANGE' });
      throws(() => target.setTimeout(Infinity), { code: 'ERR_OUT_OF_RANGE' });
    }
    strictEqual((await collect(res)).toString(), 'firstlast');
    deepStrictEqual(log, []);
  },
};
