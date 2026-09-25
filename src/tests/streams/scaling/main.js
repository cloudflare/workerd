// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the queue scaling suite. Every test is re-exported by
// name; a name collision between modules is a load-time SyntaxError, not
// a silently dropped test.

export {
  backlogReadScalesLinearly,
  teeBacklogReadScalesLinearly,
  pendingReadsScaleLinearly,
} from 'readable';

export {
  byobFillsOverBacklogScaleLinearly,
  pendingByobReadsScaleLinearly,
} from 'readable-byte';

export { unawaitedWritesScaleLinearly } from 'writable';

export { identityUnawaitedWritesScaleLinearly } from 'identity';
