// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// A stateless Worker's fetch handler answers the same with the prototype direct-dispatch path
// (WORKERD_DIRECT_DISPATCH=1 runs the handler inside hyper's dispatch, =batched one KJ event
// later) as without it: sync and async responses, request bodies and headers, streamed
// responses, exceptions, waitUntil(), HEAD and keep-alive.

import { execFileSync, spawn } from 'node:child_process';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

// /waituntil registers a waitUntil() task that finishes once the handler has returned; /waited
// waits for the last one and reports how many have run.
const WORKER = `let ran = 0;
let lastTask = Promise.resolve();
export default {
  async fetch(request, env, ctx) {
    switch (new URL(request.url).pathname) {
      case '/sync': return new Response('hello world');
      case '/headers':
        return new Response('h', { status: 201, headers: { 'x-a': '1', 'content-type': 'text/x' } });
      case '/echo': return new Response(await request.text());
      case '/echo-header': return new Response(request.headers.get('x-test') ?? 'none');
      case '/await':
        await new Promise((resolve) => setTimeout(resolve, 0));
        return new Response('awaited');
      case '/stream': {
        const { readable, writable } = new TransformStream();
        (async () => {
          const writer = writable.getWriter();
          for (let i = 0; i < 3; i++) await writer.write(new TextEncoder().encode('c' + i));
          await writer.close();
        })();
        return new Response(readable);
      }
      case '/throw': throw new Error('boom');
      case '/waituntil':
        lastTask = new Promise((resolve) => setTimeout(resolve, 0)).then(() => { ran++; });
        ctx.waitUntil(lastTask);
        return new Response('wu');
      case '/waited':
        await lastTask;
        return new Response(String(ran));
      case '/empty': return new Response(null, { status: 204 });
      case '/json': return Response.json({ a: 1 });
      case '/big': return new Response('x'.repeat(1 << 20));
      default: return new Response('nf', { status: 404 });
    }
  },
};
`;

const CONTROL_FD = 3;

// Runs workerd on `dir`'s wrangler.jsonc, with WORKERD_DIRECT_DISPATCH set to `mode` unless it
// is undefined, until it listens. Answers {port, stop}.
function serve(dir, mode) {
  const childEnv = { ...env };
  delete childEnv.WORKERD_DIRECT_DISPATCH;
  if (mode !== undefined) childEnv.WORKERD_DIRECT_DISPATCH = mode;
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      join(dir, 'wrangler.jsonc'),
      `--control-fd=${CONTROL_FD}`,
      '--socket-addr=http=127.0.0.1:0',
    ],
    { env: childEnv, stdio: ['ignore', 'inherit', 'inherit', 'pipe'] }
  );
  const exited = new Promise((resolve) => child.once('exit', resolve));
  return new Promise((resolve, reject) => {
    let buffer = '';
    child.stdio[CONTROL_FD].on('data', (d) => {
      buffer += d;
      for (const line of buffer.split('\n').slice(0, -1)) {
        const m = JSON.parse(line);
        if (m.event === 'listen') {
          resolve({ port: m.port, stop: () => (child.kill(), exited) });
        }
      }
      buffer = buffer.slice(buffer.lastIndexOf('\n') + 1);
    });
    child.once('error', reject);
    exited.then((code) => reject(new Error(`workerd exited with ${code}`)));
  });
}

async function check(dir, mode) {
  const server = await serve(dir, mode);
  const url = (path) => `http://127.0.0.1:${server.port}${path}`;
  const text = async (path, init) => {
    const r = await fetch(url(path), init);
    return [r.status, await r.text()];
  };
  try {
    assert.deepStrictEqual(await text('/sync'), [200, 'hello world']);
    const headers = await fetch(url('/headers'));
    assert.strictEqual(headers.status, 201);
    assert.strictEqual(headers.headers.get('x-a'), '1');
    assert.strictEqual(headers.headers.get('content-type'), 'text/x');
    assert.strictEqual(await headers.text(), 'h');
    assert.deepStrictEqual(
      await text('/echo', { method: 'POST', body: 'posted-body' }),
      [200, 'posted-body']
    );
    assert.deepStrictEqual(
      await text('/echo-header', { headers: { 'x-test': 'yes' } }),
      [200, 'yes']
    );
    assert.deepStrictEqual(await text('/await'), [200, 'awaited']);
    assert.deepStrictEqual(await text('/stream'), [200, 'c0c1c2']);
    assert.strictEqual((await text('/throw'))[0], 500);
    assert.deepStrictEqual(await text('/waituntil'), [200, 'wu']);
    assert.deepStrictEqual(await text('/waited'), [200, '1']);
    assert.deepStrictEqual(await text('/empty'), [204, '']);
    assert.deepStrictEqual(await text('/json'), [200, '{"a":1}']);
    assert.deepStrictEqual(await text('/big'), [200, 'x'.repeat(1 << 20)]);
    const head = await fetch(url('/sync'), { method: 'HEAD' });
    assert.strictEqual(head.status, 200);
    assert.strictEqual(head.headers.get('content-length'), '11');
    assert.strictEqual(await head.text(), '');
    // Many requests at once over several connections, mixing the paths that finish during
    // dispatch with ones that wait.
    const results = await Promise.all(
      Array.from({ length: 64 }, (_, i) => text(i % 2 ? '/sync' : '/await'))
    );
    results.forEach((r, i) =>
      assert.deepStrictEqual(r, i % 2 ? [200, 'hello world'] : [200, 'awaited'])
    );
  } finally {
    await server.stop();
  }
}

test('direct dispatch answers like the default path', async () => {
  const dir = await mkdtemp(
    join(env.TEST_TMPDIR ?? tmpdir(), 'direct-dispatch-')
  );
  const compatibilityDate = execFileSync(env.WORKERD_BINARY, ['--version'])
    .toString()
    .split(' ')[1]
    .trim();
  await writeFile(
    join(dir, 'wrangler.jsonc'),
    JSON.stringify({
      name: 'direct-dispatch',
      main: 'index.js',
      compatibility_date: compatibilityDate,
      no_bundle: true,
    })
  );
  await writeFile(join(dir, 'index.js'), WORKER);
  for (const mode of [undefined, '1', 'batched']) await check(dir, mode);
});
