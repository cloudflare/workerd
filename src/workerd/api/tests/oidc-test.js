// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strict as assert } from 'node:assert';
import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';
import unsafe from 'workerd:unsafe';

const unsupported = /OIDC token issuance is not supported by this runtime/;

async function checkOidc(ctx, enabled, isObject = false) {
  assert.equal('oidc' in ctx, enabled);
  if (!enabled) return;

  assert.equal(ctx.oidc, ctx.oidc);
  assert.equal(typeof ctx.oidc.getToken, 'function');
  await assert.rejects(async () => ctx.oidc.getToken(), unsupported);
  for (const audience of [
    'urn:example:recipient',
    'urn:example:%E9%9B%AA',
    'https://EXAMPLE.com:443/a/../b?x=%2f',
    ' two words ',
    '雪 🔐',
    'a\nb',
    'a\0b',
  ]) {
    await assert.rejects(
      async () => ctx.oidc.getToken({ audience }),
      unsupported
    );
  }
  await assert.rejects(
    async () => ctx.oidc.getToken({ audience: 'a'.repeat(2048) }),
    unsupported
  );
  await assert.rejects(
    async () => ctx.oidc.getToken({ audience: '雪'.repeat(682) + 'ab' }),
    unsupported
  );
  for (const audience of [
    '',
    ' ',
    '\t\r\n',
    '\u00a0\u3000\ufeff',
    'https://example.com/a b',
    'urn:example:雪',
    'urn:example:%zz',
    ':missing-scheme',
    '1bad:scheme',
    'https://[invalid]/',
    '\ud800',
    'a'.repeat(2049),
    '雪'.repeat(683),
    'a'.repeat(1 << 24),
  ]) {
    await assert.rejects(async () => ctx.oidc.getToken({ audience }), {
      name: 'TypeError',
      message: /OIDC audience/,
    });
  }
  for (const subject of [
    'worker:forged',
    'namespace\0',
    '雪'.repeat(1 << 22),
  ]) {
    await assert.rejects(async () => ctx.oidc.getToken({ subject }), {
      name: 'TypeError',
      message: /OIDC subject/,
    });
  }
  for (const value of [null, 42, ['one', 'two'], {}]) {
    await assert.rejects(
      async () => ctx.oidc.getToken({ audience: value }),
      TypeError
    );
    await assert.rejects(
      async () => ctx.oidc.getToken({ subject: value }),
      TypeError
    );
  }
  const uncoercible = {
    toString() {
      throw new Error('must not coerce OIDC options');
    },
  };
  for (const name of ['audience', 'subject']) {
    await assert.rejects(
      async () => ctx.oidc.getToken({ [name]: uncoercible }),
      TypeError
    );
  }
  for (const subject of ['object', 'namespace']) {
    await assert.rejects(
      async () => ctx.oidc.getToken({ subject }),
      isObject
        ? unsupported
        : { name: 'TypeError', message: /inside a Durable Object/ }
    );
  }
}

export class OidcService extends WorkerEntrypoint {
  async check() {
    await checkOidc(this.ctx, this.env.enabled);
    return true;
  }
}

export class OidcObject extends DurableObject {
  async check() {
    await checkOidc(this.ctx, this.env.enabled, true);
    return true;
  }

  async checkAlarm() {
    const completion = new Promise((resolve, reject) => {
      this.alarmComplete = { resolve, reject };
    });
    await this.ctx.storage.setAlarm(Date.now());
    await completion;
    return true;
  }

  async alarm() {
    try {
      await checkOidc(this.ctx, this.env.enabled, true);
      this.alarmComplete.resolve();
    } catch (error) {
      this.alarmComplete.reject(error);
    }
  }

  fetch() {
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, message) {
    if (message === 'close') {
      ws.close(1000, 'done');
      return;
    }
    await checkOidc(this.ctx, this.env.enabled, true);
    ws.send('checked');
  }
}

export const oidc = {
  async test(controller, env, ctx) {
    await checkOidc(ctx, env.enabled);
    assert.equal(await env.service.check(), true);
    assert.equal(await env.objects.getByName('object-a').check(), true);
    assert.equal(await env.objects.getByName('object-b').check(), true);
    const stub = env.objects.getByName('events');
    assert.equal(await stub.checkAlarm(), true);

    const response = await stub.fetch('https://example.test', {
      headers: { Upgrade: 'websocket' },
    });
    const ws = response.webSocket;
    assert(ws);
    ws.accept();
    for (let i = 0; i < 2; ++i) {
      const message = new Promise((resolve, reject) => {
        ws.addEventListener('message', (event) => resolve(event.data), {
          once: true,
        });
        ws.addEventListener('error', reject, { once: true });
      });
      ws.send('check');
      assert.equal(await message, 'checked');
      if (i === 0) await unsafe.evict(stub);
    }
    const closed = new Promise((resolve) =>
      ws.addEventListener('close', resolve, { once: true })
    );
    ws.send('close');
    await closed;
  },
};
