// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  deepStrictEqual,
  notStrictEqual,
  rejects,
  strictEqual,
} from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';
import { checkShape } from 'ctx-shape';

// The ctx of an earlier request; see reusedCtx.
let stashed;

export class Shape extends WorkerEntrypoint {
  check() {
    checkShape(this.ctx);
  }

  stash() {
    stashed = this.ctx;
  }

  // Uses the ctx of the earlier request, whose methods act on the current request.
  useStashed() {
    notStrictEqual(stashed, this.ctx);
    strictEqual(stashed.waitUntil(Promise.resolve()), undefined);
    strictEqual(stashed.passThroughOnException(), undefined);
    deepStrictEqual(stashed.props, {});
  }

  async fetch(request) {
    const reason = new URL(request.url).searchParams.get('reason');
    if (reason === 'error') this.ctx.abort(new RangeError('boom'));
    else if (reason === 'string') this.ctx.abort('str');
    else this.ctx.abort();
    // abort() throws, so this is not reached.
    return new Response();
  }
}

export const testHandlerCtx = {
  test(controller, env, ctx) {
    checkShape(ctx);
  },
};

export const entrypointCtx = {
  async test(controller, env) {
    await env.SHAPE.check();
  },
};

// The flagged service enables every flag that adds a member or changes one.
export const flaggedCtx = {
  async test(controller, env) {
    await env.FLAGGED.check();
    await env.FLAGGED.checkFlagged();
  },
};

// A ctx kept past its request still works in a later one.
export const reusedCtx = {
  async test(controller, env) {
    await env.SHAPE.stash();
    await env.SHAPE.useStashed();
  },
};

export const abortReason = {
  async test(controller, env) {
    await rejects(env.SHAPE.fetch('http://x/?reason=error'), {
      name: 'RangeError',
      message: 'boom',
    });
    await rejects(env.SHAPE.fetch('http://x/?reason=string'), {
      name: 'Error',
      message: 'str',
    });
    await rejects(env.SHAPE.fetch('http://x/'), {
      name: 'Error',
      message: 'Worker execution was aborted due to call to ctx.abort().',
    });
  },
};

// ctx.restore() reports synchronous errors as rejections under capture_async_api_throws, and
// throws them otherwise.
export const restoreErrors = {
  async test(controller, env) {
    await env.FLAGGED.checkRestoreErrors();
    await env.NO_CAPTURE.checkRestoreErrors();
  },
};
