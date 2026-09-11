// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tearing the exchange down: destroying the response cancels the body
// stream underneath, which reaches the server; the server going away
// mid-body aborts the response; and the message's own end.

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

// A consumed response ends, then closes, destroyed.
export const consumedResponseEndsThenCloses = {
  async test(ctrl, env) {
    const res = await response(get(env, '/asd'));
    const events = [];
    res.on('end', () => events.push(`end:${res.destroyed}`));
    res.on('close', () => events.push(`close:${res.destroyed}`));
    res.resume();
    await once(res, 'close');
    strictEqual(events[0], 'end:false');
    strictEqual(events[1], 'close:true');
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
