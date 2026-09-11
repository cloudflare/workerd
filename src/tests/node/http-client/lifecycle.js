// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tearing the exchange down: destroying the request aborts the fetch (or
// cancels the response body) and reports it as Node does; destroying the
// response cancels the body stream underneath, which reaches the server;
// the server going away mid-body aborts the response; a failed connection
// errors the request; and the request closes once the exchange is over.

import { strictEqual, deepStrictEqual, ok } from 'node:assert';
import {
  request,
  get,
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

// A completed exchange closes the request after the response ends, and
// leaves it destroyed.
export const responseEndClosesRequest = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, '/asd');
    record(log, 'req', req, ['finish', 'error', 'close']);
    const res = await response(req);
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
