// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

export const success = {
  async test(_controller, env) {
    const response = await env.target.fetch('http://origin/success');
    assert.equal(response.status, 200);
    await response.text();
  },
};

export const redirects = {
  async test(_controller, env) {
    const response = await env.target.fetch('http://origin/start?hop=1');
    assert.equal(response.url, 'http://other/final?hop=3');
    assert.equal(response.status, 200);
    await response.text();
  },
};

export const manualRedirect = {
  async test(_controller, env) {
    const response = await env.target.fetch('http://origin/start?manual=1', {
      redirect: 'manual',
    });
    assert.equal(response.status, 302);
    await response.text();
  },
};

export const errorResponses = {
  async test(_controller, env) {
    for (const status of [404, 503]) {
      const response = await env.target.fetch(`http://origin/status/${status}`);
      assert.equal(response.status, status);
      await response.text();
    }
  },
};

export const connectionFailure = {
  async test(_controller, env) {
    await assert.rejects(
      env.unreachable.fetch('http://origin/unreachable'),
      (error) => {
        assert.equal(error.name, 'Error');
        assert.ok(error.message.length > 0);
        console.log('fetch-error', error.name, error.message);
        return true;
      }
    );
  },
};
