// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Module identity across a startup snapshot. A case Worker's top level imports a bundle ES
// module, a JSON module and node:buffer, then at request time looks each of them up again the
// way runtime code does (import(), process.getBuiltinModule(), the Buffer global) and reports
// whether it got the instances its top level evaluated against: one module graph means a
// module's top level ran once, instanceof holds across the two paths and the lookups return the
// same namespace objects. The runner asserts the reports. Under the startup-snapshot autogate
// every service here goes through the zygote; without it they are plain Workers and the same
// assertions hold with snapshotUsed false.

import { strictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';
import { env } from 'cloudflare:workers';
import * as lib from './lib.js';
import data from './data.json';

const MODE = env.MODE;

// Whether this Worker was started from a startup snapshot rather than running its top level
// itself. The zygote's env carries no service bindings (they are deferred to the started Worker:
// isDeferredForSnapshot in server/workerd-api.c++), so at module scope SELF is undefined exactly
// when this code runs in the zygote, and the constant is baked into the snapshot.
const snapshotUsed = typeof env.SELF === 'undefined';

// Top-level state the request-time lookups are compared against.
const topLevelBuffer = Buffer.from('x');
const resolvedAtTopLevel = resolveLib();

// The legacy module registry installs no import.meta.resolve.
function resolveLib() {
  return typeof import.meta.resolve === 'function'
    ? import.meta.resolve('./lib.js')
    : null;
}

async function checks() {
  const dynLib = await import('./lib.js');
  const dynData = await import('./data.json');
  const dynBuffer = await import('node:buffer');
  return {
    // The Buffer global is resolved from node:buffer on first access (getBuffer in
    // api/global-scope.c++); isBuffer is an instanceof check against that copy's prototype.
    bufferIsBuffer: globalThis.Buffer.isBuffer(topLevelBuffer),
    bufferGlobal: Buffer === globalThis.Buffer,
    bufferDynamicImport: dynBuffer.Buffer === Buffer,
    bufferBuiltinModule:
      process.getBuiltinModule('node:buffer').Buffer === Buffer,
    libNamespace: dynLib === lib,
    libToken: dynLib.token === lib.token,
    libEvaluatedOnce: globalThis.__libEvaluations === 1,
    jsonDefault: dynData.default === data,
    importMetaResolve: resolveLib() === resolvedAtTopLevel,
  };
}

export async function report(extraChecks = {}) {
  return {
    mode: MODE,
    snapshotUsed,
    checks: { ...(await checks()), ...extraChecks },
  };
}

async function fetch() {
  return Response.json(await report());
}

async function test(ctrl, env) {
  // The runner went through the same pipeline as the cases: whether it was started from a
  // snapshot says whether the pipeline is on in this variant.
  const pipelineOn = snapshotUsed;
  const run = async (name, expectedSnapshotUsed) => {
    const response = await env[name].fetch('http://case/');
    const r = await response.json();
    strictEqual(r.mode, name);
    strictEqual(r.snapshotUsed, expectedSnapshotUsed, `${name}: snapshotUsed`);
    for (const [check, value] of Object.entries(r.checks)) {
      strictEqual(value, true, `${name}: ${check}`);
    }
  };
  await run('case', pipelineOn);
  await run('case-cjs', pipelineOn);
}

// Only the runner has a test handler: workerd test runs test() of every service that exports one.
export default MODE === 'runner' ? { fetch, test } : { fetch };
