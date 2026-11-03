// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The case of startup-snapshot-new-module-registry-test.js.

import { env } from 'cloudflare:workers';
import * as lib from './lib.js';
import data from './data.json';

// Whether this Worker was started from a startup snapshot rather than running its top level
// itself. In the zygote's env a service binding is an `undefined` placeholder (the started Worker
// supplies the value: isDeferredForSnapshot in server/workerd-api.c++), so at module scope SELF
// is undefined exactly when this code runs in the zygote, and the constant is baked into the
// snapshot.
const snapshotUsed = typeof env.SELF === 'undefined';

async function checks() {
  const dynLib = await import('./lib.js');
  const dynData = await import('./data.json');
  // Not loaded at the top level: resolved, evaluated and given its import.meta at request time.
  const lazy = await import('./lazy.js');
  return {
    libNamespace: dynLib === lib,
    libEvaluatedOnce: globalThis.__libEvaluations === 1,
    jsonDefault: dynData.default === data,
    lazyLinksToLib: lazy.token === lib.token,
    lazyUrl: lazy.url.endsWith('/lazy.js'),
    lazyNotMain: lazy.main === false,
    // The main module's import.meta is first read here, at request time.
    mainUrl: import.meta.url.endsWith('/worker.js'),
    mainIsMain: import.meta.main === true,
  };
}

export default {
  async fetch() {
    return Response.json({ snapshotUsed, checks: await checks() });
  },
};
