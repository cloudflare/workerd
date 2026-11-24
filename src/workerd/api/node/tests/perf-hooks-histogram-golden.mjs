// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Generates perf-hooks-histogram-golden.json, Node.js's results for the scenarios in
// perf-hooks-histogram-scenarios.js, which perf-hooks-histogram-nodejs-test.js compares against:
//
//   node src/workerd/api/node/tests/perf-hooks-histogram-golden.mjs \
//     > src/workerd/api/node/tests/perf-hooks-histogram-golden.json
//
// Run it with an official x86-64 build of Node.js 26.10 or later (under Rosetta on Apple silicon).
// Node.js's arm64 builds fuse multiplies and adds, so their means and standard deviations can
// differ in the last bit; workerd, like the x86-64 builds, does not. Distribution builds that
// link a system HdrHistogram_c (such as Homebrew's) differ more, for example on empty histograms.

import { run } from './perf-hooks-histogram-scenarios.js';

process.stdout.write(
  JSON.stringify({ node: process.version, results: await run() }, null, 1) +
    '\n'
);
