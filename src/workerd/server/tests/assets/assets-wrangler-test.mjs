// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of a wrangler.jsonc with assets: the router sends each
// request to a file or to the Worker, `_redirects`, `_headers` and
// `.assetsignore` apply, `env.ASSETS` serves files, a project without
// `main` serves only files, and without --experimental the server refuses
// to start and says why.

import { spawn } from 'node:child_process';
import { request } from 'node:http';
import { mkdir, mkdtemp, stat, utimes, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

const WORKER = `export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/via-binding') {
      return env.ASSETS.fetch(new URL('/about', url));
    }
    return new Response('worker ' + url.pathname);
  },
};
`;

const FILES = {
  'index.html': '<h1>home</h1>',
  'about.html': '<h1>about</h1>',
  'blog/index.html': '<h1>post</h1>',
  'style.css': 'body {}',
  '404.html': '<h1>missing</h1>',
  '.well-known/security.txt': 'contact',
  'secret/key.txt': 'hidden',
  _redirects: '/old /about 301\n/proxy /about 200\n',
  _headers: '/*\n  X-Site: yes\n',
  '.assetsignore': 'secret/\n',
};

const CONTROL_FD = 3;

// Runs workerd on `dir`'s wrangler.jsonc until it listens, or until it
// exits. Answers {port, stop} or {code, stderr}.
function serve(dir, args) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      join(dir, 'wrangler.jsonc'),
      `--control-fd=${CONTROL_FD}`,
      '--socket-addr=http=127.0.0.1:0',
      ...args,
    ],
    { stdio: ['ignore', 'inherit', 'pipe', 'pipe'] }
  );
  let stderr = '';
  child.stderr.on('data', (d) => {
    stderr += d;
    process.stderr.write(d);
  });
  // `close`, unlike `exit`, waits for stderr to be drained.
  const exited = new Promise((resolve) => child.once('close', resolve));
  return new Promise((resolve, reject) => {
    let buffer = '';
    child.stdio[CONTROL_FD].on('data', (d) => {
      buffer += d;
      for (const line of buffer.split('\n').slice(0, -1)) {
        const m = JSON.parse(line);
        if (m.event === 'listen') {
          const stop = () => (child.kill(), exited);
          resolve({ port: m.port, stop });
        }
      }
      buffer = buffer.slice(buffer.lastIndexOf('\n') + 1);
    });
    child.once('error', reject);
    exited.then((code) => resolve({ code, stderr }));
  });
}

async function project(wrangler, worker) {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'assets-'));
  await writeFile(join(dir, 'wrangler.jsonc'), JSON.stringify(wrangler));
  if (worker) await writeFile(join(dir, 'index.js'), worker);
  for (const [path, text] of Object.entries(FILES)) {
    await mkdir(dirname(join(dir, 'public', path)), { recursive: true });
    await writeFile(join(dir, 'public', path), text);
  }
  return dir;
}

const get = (port, path, init) =>
  fetch(`http://127.0.0.1:${port}${path}`, { redirect: 'manual', ...init });

test('assets with a Worker', async (t) => {
  const dir = await project(
    {
      name: 'site',
      main: 'index.js',
      compatibility_date: '2026-01-01',
      no_bundle: true,
      assets: {
        directory: 'public',
        binding: 'ASSETS',
        not_found_handling: '404-page',
      },
    },
    WORKER
  );
  const refused = await serve(dir, []);
  assert.notStrictEqual(refused.code, 0);
  assert.match(refused.stderr, /assets.*--experimental/s);

  const server = await serve(dir, ['--experimental']);
  t.after(server.stop);
  const expect = async (path, status, body, init) => {
    const r = await get(server.port, path, init);
    assert.strictEqual(r.status, status, path);
    if (typeof body === 'string')
      assert.strictEqual(await r.text(), body, path);
    return r;
  };
  const home = await expect('/', 200, FILES['index.html']);
  assert.strictEqual(
    home.headers.get('content-type'),
    'text/html; charset=utf-8'
  );
  assert.strictEqual(home.headers.get('x-site'), 'yes');
  assert.strictEqual(
    home.headers.get('cache-control'),
    'public, max-age=0, must-revalidate'
  );
  await expect('/about', 200, FILES['about.html']);
  const html = await expect('/about.html', 307);
  assert.strictEqual(html.headers.get('location'), '/about');
  await expect('/blog/', 200, FILES['blog/index.html']);
  assert.strictEqual(
    (await expect('/blog', 307)).headers.get('location'),
    '/blog/'
  );
  const css = await expect('/style.css', 200, FILES['style.css']);
  assert.strictEqual(
    css.headers.get('content-type'),
    'text/css; charset=utf-8'
  );
  const etag = css.headers.get('etag');
  await expect('/style.css', 304, '', { headers: { 'If-None-Match': etag } });
  const head = await expect('/style.css', 200, '', { method: 'HEAD' });
  assert.strictEqual(
    head.headers.get('content-length'),
    String(FILES['style.css'].length)
  );
  await expect('/.well-known/security.txt', 200, 'contact');

  // No file: the Worker, also for what is ignored; but not for a
  // navigation, which gets the 404 page.
  await expect('/missing', 200, 'worker /missing');
  // Not with fetch, which sets its own Sec-Fetch-Mode.
  const navigation = await new Promise((resolve, reject) => {
    const headers = { 'Sec-Fetch-Mode': 'navigate' };
    const url = `http://127.0.0.1:${server.port}/missing`;
    request(url, { headers }, (r) => {
      let body = '';
      r.on('data', (d) => (body += d));
      r.on('end', () => resolve({ status: r.statusCode, body }));
    })
      .on('error', reject)
      .end();
  });
  assert.deepStrictEqual(navigation, { status: 404, body: FILES['404.html'] });
  await expect('/secret/key.txt', 200, 'worker /secret/key.txt');
  // An image optimizer fetching another host is refused before the Worker.
  await expect('/_image?href=//evil.com/x', 403, 'Blocked');
  for (const file of ['_headers', '_redirects', '.assetsignore']) {
    await expect(`/${file}`, 200, `worker /${file}`);
  }

  assert.strictEqual(
    (await expect('/old', 301)).headers.get('location'),
    '/about'
  );
  await expect('/proxy', 200, FILES['about.html']);
  await expect('/via-binding', 200, FILES['about.html']);
  await expect('/about', 405, '', { method: 'POST' });

  // A file appears at once.
  await writeFile(join(dir, 'public', 'new.txt'), 'new');
  await expect('/new.txt', 200, 'new');
  // As does an edit of the same size within the same second.
  const path = join(dir, 'public', 'style.css');
  const { mtimeMs } = await stat(path);
  await writeFile(path, 'body{} ');
  const second = Math.floor(mtimeMs / 1000) * 1000;
  const mtime = second + (mtimeMs - second === 500 ? 250 : 500);
  await utimes(path, new Date(), new Date(mtime));
  const edited = await expect('/style.css', 200, 'body{} ', {
    headers: { 'If-None-Match': etag },
  });
  assert.notStrictEqual(edited.headers.get('etag'), etag);
});

test('run_worker_first', async (t) => {
  const wrangler = (run_worker_first) => ({
    main: 'index.js',
    compatibility_date: '2026-01-01',
    no_bundle: true,
    assets: { directory: 'public', run_worker_first },
  });
  for (const [first, css, about] of [
    [['/*.css'], 'worker /style.css', FILES['about.html']],
    [['/*', '!/about'], 'worker /style.css', FILES['about.html']],
    [true, 'worker /style.css', 'worker /about'],
  ]) {
    const server = await serve(await project(wrangler(first), WORKER), [
      '--experimental',
    ]);
    t.after(server.stop);
    const text = async (path) => (await get(server.port, path)).text();
    assert.strictEqual(await text('/style.css'), css);
    assert.strictEqual(await text('/about'), about);
  }
});

test('assets without a Worker', async () => {
  const dir = await project({
    compatibility_date: '2026-01-01',
    assets: {
      directory: 'public',
      not_found_handling: 'single-page-application',
    },
  });
  const server = await serve(dir, ['--experimental']);
  const r = await get(server.port, '/no/such/page');
  assert.strictEqual(r.status, 200);
  assert.strictEqual(await r.text(), FILES['index.html']);
  await server.stop();

  const bad = await project(
    {
      main: 'index.js',
      compatibility_date: '2026-01-01',
      no_bundle: true,
      assets: { directory: 'public', run_worker_first: ['api/*'] },
    },
    WORKER
  );
  const failed = await serve(bad, ['--experimental']);
  assert.notStrictEqual(failed.code, 0);
  assert.match(failed.stderr, /rules must start with '\/'/);

  const missing = await project({
    compatibility_date: '2026-01-01',
    assets: { directory: 'nowhere' },
  });
  const refused = await serve(missing, ['--experimental']);
  assert.notStrictEqual(refused.code, 0);
  assert.match(refused.stderr, /assets\.directory: .*nowhere: not a directory/);
});
