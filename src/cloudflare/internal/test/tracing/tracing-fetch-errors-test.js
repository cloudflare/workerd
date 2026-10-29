// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

export const networkFailure = {
  async test(_controller, env) {
    await assert.rejects(env.unreachable.fetch('http://origin/unreachable'), {
      name: 'Error',
      message: 'Network connection lost.',
    });
  },
};

export const abort = {
  async test(_controller, env) {
    const controller = new AbortController();
    const response = env.target.fetch('http://origin/abort', {
      signal: controller.signal,
    });
    controller.abort('cancelled');
    await assert.rejects(response, { name: 'Error', message: 'cancelled' });
  },
};

export const manualRedirect = {
  async test(_controller, env) {
    const response = await env.target.fetch('http://origin/manual', {
      redirect: 'manual',
    });
    assert.equal(response.status, 302);
    await response.text();
  },
};

export const tooManyRedirects = {
  async test(_controller, env) {
    await assert.rejects(env.target.fetch('http://origin/loop?n=0'), {
      name: 'TypeError',
      message: /^Too many redirects\./,
    });
  },
};

export const longRedirectError = {
  async test(_controller, env) {
    await assert.rejects(
      env.target.fetch(`http://origin/loop?n=0&padding=${'x'.repeat(8192)}`),
      (error) =>
        error.name === 'TypeError' &&
        error.message.startsWith('Too many redirects.') &&
        error.message.length > 4096
    );
  },
};

export const invalidLocation = {
  async test(_controller, env) {
    await assert.rejects(env.target.fetch('http://origin/bad-location'), {
      name: 'TypeError',
      message: 'Invalid Location header; unable to follow redirect.',
    });
  },
};

export const laterHopFailure = {
  async test(_controller, env) {
    await assert.rejects(env.target.fetch('http://origin/redirect-bad'), {
      name: 'TypeError',
      message: 'Invalid Location header; unable to follow redirect.',
    });
  },
};

export const noUserGetters = {
  async test(_controller, env) {
    const descriptor = Object.getOwnPropertyDescriptor(Error.prototype, 'name');
    const stackDescriptor = Object.getOwnPropertyDescriptor(
      Error,
      'prepareStackTrace'
    );
    let calls = 0;
    Object.defineProperty(Error.prototype, 'name', {
      configurable: true,
      get() {
        calls++;
        throw new Error('getter must not run');
      },
    });
    Object.defineProperty(Error, 'prepareStackTrace', {
      configurable: true,
      get() {
        calls++;
        throw new Error('stack formatting must not run');
      },
    });
    try {
      let rejected = false;
      try {
        await env.unreachable.fetch('http://origin/getter');
      } catch (error) {
        rejected = true;
        assert.equal(error.message, 'Network connection lost.');
      }
      assert.ok(rejected);
      assert.equal(calls, 0);
    } finally {
      Object.defineProperty(Error.prototype, 'name', descriptor);
      if (stackDescriptor) {
        Object.defineProperty(Error, 'prepareStackTrace', stackDescriptor);
      } else {
        delete Error.prepareStackTrace;
      }
    }
  },
};

// Pin the known gap: failed WebSocket upgrades do not yet classify their fetch spans as errors.
export const webSocketFailure = {
  async test(_controller, env) {
    await assert.rejects(
      env.unreachable.fetch('http://origin/websocket', {
        headers: { upgrade: 'websocket' },
      }),
      { name: 'Error', message: 'Network connection lost.' }
    );
  },
};

export const success = {
  async test(_controller, env) {
    const response = await env.target.fetch('http://origin/success');
    assert.equal(response.status, 200);
    await response.text();
  },
};
