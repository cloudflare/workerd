// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of the Workflows engine (`/workerd/workflows.js`), killed
// and started again.

import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

// `/create?id=&kind=`, `/until?id=&type=&n=` (once the instance's history
// has n events of \`type\`), `/pause?id=`, `/status?id=` and
// `/clock?advance=<ms>`. A \`hang\` step never returns.
const WORKER = `import { WorkflowEntrypoint } from 'cloudflare:workers';
export class Flow extends WorkflowEntrypoint {
  async run(event, step) {
    const hang = () => new Promise(() => {});
    if (event.payload === 'crash') {
      const retries = { limit: 1, delay: 0 };
      return step.do('crash', { retries }, hang);
    }
    if (event.payload === 'hang') return step.do('hang', hang);
    await step.do('one', async () => console.log('RAN one'));
    await step.sleep('nap', '1 minute');
    await step.do('two', async () => console.log('RAN two'));
    return 'done';
  }
}
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const id = url.searchParams.get('id');
    if (url.pathname === '/clock') return env.WORKFLOWS.fetch(request);
    if (url.pathname === '/create') {
      const params = url.searchParams.get('kind');
      return Response.json((await env.FLOW.create({ id, params })).id);
    }
    const instance = await env.FLOW.get(id);
    if (url.pathname === '/pause') return Response.json(await instance.pause() ?? null);
    if (url.pathname === '/status') {
      return Response.json(await instance.status());
    }
    const type = url.searchParams.get('type');
    let n = Number(url.searchParams.get('n') ?? 1);
    const subscription = await instance.subscribe();
    for (;;) {
      const { done, value } = await subscription.next();
      if (done) return new Response('ended', { status: 500 });
      if (value.type === type && --n === 0) return Response.json(value);
    }
  },
};
`;

const flow = `( name = "workflows", entrypoint = "WorkflowBinding",
  props = ( json = "{\\"workflow\\": \\"flow\\"}" ) )`;

const config = (dir) => `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [
    ( name = "store",
      disk = ( path = ${JSON.stringify(join(dir, 'store'))},
               writable = true ) ),
    ( name = "app",
      worker = (
        modules = [ ( name = "app.js", esModule = embed "app.js" ) ],
        compatibilityDate = "2026-01-01",
        bindings = [
          ( name = "FLOW", wrapped = (
              moduleName = "cloudflare-internal:workflows-api",
              innerBindings = [ ( name = "fetcher", service = ${flow} ) ] ) ),
          ( name = "WORKFLOWS", service = "workflows" ),
        ],
        workflowsEngine = (
          actorClass = ( name = "workflows", entrypoint = "Engine" ),
          workflows = [
            ( className = "Flow", name = "flow", bindingService = ${flow} ) ],
        ),
      )
    ),
    ( name = "workflows",
      worker = (
        modules = [
          ( name = "workflows.js", bundled = "/workerd/workflows.js" ) ],
        compatibilityDate = "2026-01-01",
        bindings = [
          ( name = "flow", durableObjectNamespace = (
              className = "miniflare-workflows-flow", serviceName = "app" ) ),
          ( name = "TEST_CLOCK", json = "true" ),
        ],
        durableObjectStorage = ( localDisk = "store" ),
      )
    ),
  ],
  sockets = [
    ( name = "http", address = "127.0.0.1:0", http = (), service = "app" ) ],
);
`;

// Runs workerd on `dir`'s config. `ran` holds each step that logged it ran;
// `get(path)` fetches from the app.
function serve(dir) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      join(dir, 'config.capnp'),
      '--experimental',
      '--control-fd=3',
      '--socket-addr=http=127.0.0.1:0',
    ],
    { stdio: ['ignore', 'pipe', 'pipe', 'pipe'] }
  );
  const port = new Promise((resolve) => {
    let control = '';
    child.stdio[3].setEncoding('utf8').on('data', (d) => {
      control += d;
      const m = control.match(/"event":"listen".*"port":(\d+)/);
      if (m) resolve(Number(m[1]));
    });
  });
  const get = async (path, method = 'GET') => {
    const r = await fetch(`http://127.0.0.1:${await port}${path}`, { method });
    assert.strictEqual(r.status, 200, `${path}: ${r.status}`);
    return r.json();
  };
  const ran = [];
  const read = (stream) => {
    let line = '';
    stream.setEncoding('utf8').on('data', (d) => {
      process.stderr.write(d);
      const lines = (line + d).split('\n');
      line = lines.pop();
      for (const l of lines) {
        const m = l.match(/RAN (\S+)/);
        if (m) ran.push(m[1]);
      }
    });
  };
  read(child.stdout);
  read(child.stderr);
  const exited = new Promise((resolve) => child.once('exit', resolve));
  return { ran, get, stop: (signal) => (child.kill(signal), exited) };
}

test('a restart goes on from a sleep, done steps not run again', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'workerd-workflows-'));
  await mkdir(join(dir, 'store'));
  await writeFile(join(dir, 'app.js'), WORKER);
  await writeFile(join(dir, 'config.capnp'), config(dir));
  let server = serve(dir);
  try {
    await server.get('/create?id=nap');
    await server.get('/until?id=nap&type=sleep_started');
    assert.deepEqual(server.ran, ['one']);
    await server.stop('SIGKILL');

    server = serve(dir);
    await server.get('/clock?advance=60000', 'POST');
    const done = await server.get('/until?id=nap&type=workflow_completed');
    assert.strictEqual(done.output, 'done');
    assert.deepEqual(server.ran, ['two']);
  } finally {
    await server.stop();
    await rm(dir, { recursive: true });
  }
});

test('an attempt that ends the process fails, a pause waits no more', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'workerd-workflows-'));
  await mkdir(join(dir, 'store'));
  await writeFile(join(dir, 'app.js'), WORKER);
  await writeFile(join(dir, 'config.capnp'), config(dir));
  let server = serve(dir);
  try {
    await server.get('/create?id=crash&kind=crash');
    await server.get('/create?id=hang&kind=hang');
    await server.get('/until?id=crash&type=attempt_started');
    await server.get('/until?id=hang&type=attempt_started');
    await server.get('/pause?id=hang');
    const { status } = await server.get('/status?id=hang');
    assert.strictEqual(status, 'waitingForPause');
    await server.stop('SIGKILL');

    // The second and last attempt.
    server = serve(dir);
    await server.get('/until?id=crash&type=attempt_started&n=2');
    assert.strictEqual((await server.get('/status?id=hang')).status, 'paused');
    await server.stop('SIGKILL');

    server = serve(dir);
    const { error } = await server.get('/until?id=crash&type=workflow_errored');
    assert.match(error.message, /Attempt 2 of step "crash" did not finish/);
  } finally {
    await server.stop();
    await rm(dir, { recursive: true });
  }
});
