// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { readFileSync } from 'node:fs';

// This module is dynamically imported by process-nodejs-test.js. Its top level
// runs while the module is being evaluated for a dynamic import, outside of any
// IoContext, so the working directory must still be /bundle here (see #8786).
if (process.cwd() !== '/bundle') {
  throw new Error(`expected cwd to be /bundle, got ${process.cwd()}`);
}

// chdir() must work here too: without a scope to hold the new cwd it threw
// EPERM.
process.chdir('/tmp');
if (process.cwd() !== '/tmp') {
  throw new Error(`expected cwd to be /tmp, got ${process.cwd()}`);
}
process.chdir('/bundle');

// Relative reads resolve against the cwd, so this must find the bundle module
// 'mod' instead of failing on '//mod'.
const modSource = readFileSync('mod', 'utf8');
if (!modSource.includes('process.env')) {
  throw new Error(`unexpected content reading 'mod': ${modSource}`);
}

export const LAZY = true;
