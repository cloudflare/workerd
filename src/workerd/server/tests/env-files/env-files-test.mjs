// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd dev` and `workerd serve --env-file`: the variables of .env files
// become a wrangler config's string bindings and join workerd's environment,
// as wrangler dev reads them.

import { mkdir, mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';
import { serve } from '../wrangler-serve.mjs';

const WRANGLER = {
  main: 'index.js',
  compatibility_date: '2026-01-01',
  no_bundle: true,
  vars: { V: 'v', A: 'from-vars' },
  secrets_store_secrets: [{ binding: 'S', store_id: 'x', secret_name: 'sec' }],
};

// GET / answers the string bindings, and as S the variable WORKERD_SECRET_SEC
// of workerd's environment.
const WORKER = `export default {
  async fetch(request, env) {
    const strings = Object.entries(env).filter(([, v]) => typeof v === 'string');
    let s;
    try {
      s = await env.S.get();
    } catch (e) {
      s = e.message;
    }
    return Response.json({ ...Object.fromEntries(strings), S: s });
  },
};
`;

// A directory holding .env and project/, a wrangler project with .dev.vars
// and .env.
async function setup() {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'env-files-'));
  const project = join(dir, 'project');
  await mkdir(project);
  await writeFile(join(project, 'wrangler.jsonc'), JSON.stringify(WRANGLER));
  await writeFile(join(project, 'index.js'), WORKER);
  await writeFile(join(project, '.dev.vars'), 'A=dev\nB=$A\n');
  await writeFile(join(project, '.env'), 'A=env\n');
  await writeFile(join(dir, '.env'), 'WORKERD_SECRET_SEC=one\n');
  return { dir, project };
}

const get = async (port) => (await fetch(`http://127.0.0.1:${port}/`)).json();

test('workerd dev', async () => {
  const { dir, project } = await setup();
  const server = await serve(project, [], {}, { command: 'dev', cwd: dir });
  // .dev.vars alone, unexpanded, over vars; .env of the current directory in
  // the environment.
  const want = { A: 'dev', B: '$A', V: 'v', S: 'one' };
  assert.deepStrictEqual(await get(server.port), want);
  if (process.platform !== 'win32') {
    // The reload reads .env again, into the environment workerd started with.
    const port = server.relisten();
    await writeFile(join(dir, '.env'), 'WORKERD_SECRET_SEC=two\n');
    assert.deepStrictEqual(await get(await port), { ...want, S: 'two' });
  }
  await server.stop();
  assert.match(server.stderr(), /Using secrets defined in project.\.dev\.vars/);
});

test('workerd serve --env-file', async () => {
  const { dir, project } = await setup();
  let server = await serve(project, ['--experimental'], {}, { cwd: dir });
  const none = { A: 'from-vars', V: 'v', S: 'Secret "sec" not found' };
  assert.deepStrictEqual(await get(server.port), none);
  await server.stop();

  await writeFile(
    join(dir, 'x.env'),
    'A=x\nB=${A}y\nH=$HOME\nWORKERD_SECRET_SEC=file\n'
  );
  const vars = { WORKERD_SECRET_SEC: 'process', HOME: '/home' };
  const args = ['--experimental', '--env-file=missing.env', '--env-file=x.env'];
  server = await serve(project, args, vars, { cwd: dir });
  // Bindings are expanded without the process's environment, and a variable
  // the process already has keeps its value.
  assert.deepStrictEqual(await get(server.port), {
    A: 'x',
    B: 'xy',
    H: '',
    V: 'v',
    WORKERD_SECRET_SEC: 'file',
    S: 'process',
  });
  await server.stop();
});
