// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Stands in for node:fs in the Node.js benchmarks that read files from
// benchmark/fixtures. run.py bundles those files as data modules under
// fixtures/, and rewrites the benchmarks' require('fs') to load this module.

'use strict';

const { Buffer } = require('buffer');

exports.readFileSync = function (path) {
  const match = /^\/fixtures\/([\w.-]+)$/.exec(path);
  if (match === null) {
    throw new Error(`${path} is not a bundled benchmark fixture`);
  }
  return Buffer.from(require(`./fixtures/${match[1]}`));
};
