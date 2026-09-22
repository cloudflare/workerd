// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entering the request and response from their own events: destroying
// or aborting from 'response', 'timeout' and 'aborted', re-arming the
// timeout from 'timeout'.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { get, once, record, uniqueId } from 'harness';

// req.destroy() from inside 'response': the response, just handed over, is
// aborted — no 'socket hang up', the response exists — and both close.
export const destroyInsideResponse = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, `/never-ends?id=${uniqueId('reenter-response')}`);
    record(log, 'req', req, ['error', 'close']);
    req.on('response', (res) => {
      record(log, 'res', res, ['aborted', 'error', 'end', 'close']);
      log.push('response');
      req.destroy();
    });
    await once(req, 'close');
    await scheduler.wait(10);
    deepStrictEqual(log, [
      'response',
      'res:aborted',
      'req:close',
      'res:error(Error/ECONNRESET/aborted)',
      'res:close',
    ]);
  },
};

// req.abort() from inside 'timeout': the quiet teardown wins over the
// timer's own (the AbortError destroy finds the request destroyed) — the
// response is aborted at once, then hears its 'timeout', 'abort' fires,
// and both close with ECONNRESET on the response, no AbortError anywhere.
export const abortInsideTimeout = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, `/never-ends?id=${uniqueId('reenter-timeout')}`);
    record(log, 'req', req, ['abort', 'error', 'close']);
    req.setTimeout(40, () => {
      log.push('timeout');
      req.abort();
    });
    const res = await once(req, 'response');
    record(log, 'res', res, ['timeout', 'aborted', 'error', 'close']);
    res.resume();
    await Promise.all([once(req, 'close'), once(res, 'close')]);
    deepStrictEqual(log, [
      'timeout',
      'res:aborted',
      'res:timeout',
      'req:abort',
      'req:close',
      'res:error(Error/ECONNRESET/aborted)',
      'res:close',
    ]);
  },
};

// req.setTimeout() from inside 'timeout' re-arms nothing that survives:
// the firing timer tears the request down right after its listeners ran,
// and destroy() clears the timer — one 'timeout', then the AbortError.
export const setTimeoutInsideTimeoutDoesNotOutliveTeardown = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, `/never-ends?id=${uniqueId('reenter-rearm')}`);
    record(log, 'req', req, ['timeout', 'error', 'close']);
    req.on('timeout', () => req.setTimeout(20));
    req.setTimeout(40);
    const res = await once(req, 'response');
    res.resume();
    await once(req, 'close');
    await scheduler.wait(80);
    deepStrictEqual(log, [
      'req:timeout',
      'req:error(AbortError/ABORT_ERR/The operation was aborted)',
      'req:close',
    ]);
  },
};

// res.destroy(err) from inside the response's 'aborted' (the request being
// destroyed): the response is already being destroyed with ECONNRESET, so
// the second destroy changes nothing — one 'error', with the first reason.
export const responseDestroyInsideAborted = {
  async test(ctrl, env) {
    const log = [];
    const req = get(env, `/never-ends?id=${uniqueId('reenter-aborted')}`);
    record(log, 'req', req, ['error', 'close']);
    const res = await once(req, 'response');
    record(log, 'res', res, ['error', 'close']);
    res.on('aborted', () => {
      log.push('aborted');
      res.destroy(new Error('again'));
    });
    res.resume();
    req.destroy();
    await Promise.all([once(req, 'close'), once(res, 'close')]);
    deepStrictEqual(log, [
      'aborted',
      'req:close',
      'res:error(Error/ECONNRESET/aborted)',
      'res:close',
    ]);
    strictEqual(res.errored.code, 'ECONNRESET');
  },
};
