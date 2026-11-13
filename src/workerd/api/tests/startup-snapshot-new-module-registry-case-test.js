// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The case of startup-snapshot-new-module-registry-test.js.

import { env } from 'cloudflare:workers';
import unsafe from 'workerd:unsafe';
import * as lib from './lib.js';
import data from './data.json';
// The registry records a resolution under its full specifier, query and fragment included, and
// restores it next to the definition it names (see IsolateModuleRegistry::restoreFromSnapshot()).
import * as variantQuery from './variant.js?q';
import * as variantFragment from './variant.js#f';
import * as buffer from 'node:buffer';

async function checks() {
  const variantEvaluations = globalThis.__variantEvaluations;
  const dynLib = await import('./lib.js');
  const dynData = await import('./data.json');
  const dynVariantQuery = await import('./variant.js?q');
  const dynVariantFragment = await import('./variant.js#f');
  const dynBuffer = await import('node:buffer');
  // V8 reports a module's canonical URL as the referrer of an import() in it, which for these is
  // file:///bundle/variant.js; the registry maps that back to the specifier it was imported under.
  const libFromVariantQuery = await variantQuery.load();
  const libFromVariantFragment = await variantFragment.load();
  const dynWorkers = await import('cloudflare:workers');
  // Not loaded at the top level: resolved, evaluated and given its import.meta at request time.
  const lazy = await import('./lazy.js');
  return {
    libNamespace: dynLib === lib,
    libEvaluatedOnce: globalThis.__libEvaluations === 1,
    jsonDefault: dynData.default === data,
    variantQueryNamespace: dynVariantQuery === variantQuery,
    variantFragmentNamespace: dynVariantFragment === variantFragment,
    variantQueryUrl: variantQuery.url.endsWith('/variant.js?q'),
    variantFragmentUrl: variantFragment.url.endsWith('/variant.js#f'),
    // import.meta.resolve, taken at the top level, is a function the snapshot carries.
    variantResolve: variantQuery.resolve('./lib.js').endsWith('/lib.js'),
    variantQueryImports: libFromVariantQuery === lib,
    variantFragmentImports: libFromVariantFragment === lib,
    variantNotReEvaluated:
      globalThis.__variantEvaluations === variantEvaluations,
    builtinNamespace: dynBuffer === buffer,
    workersEnv: dynWorkers.env === env,
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
    return Response.json({
      snapshotUsed: unsafe.startedFromStartupSnapshot(),
      checks: await checks(),
    });
  },
};
