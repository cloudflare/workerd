// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the iocontext suite. Explicit named re-exports plus
// the module's routing fetch handler (the module-scope streams live in
// global-scope-streams.js and must stay there — moving them here would
// change which module's evaluation constructs them).

export {
  globalScopeReadablestream,
  globalScopeReadablestream2,
  globalScopeReadablestream3,
  globalScopeReadablestream4,
  globalScopeReadablestream5,
  globalScopeReadablestream6,
  globalScopeReadablestream7,
  globalScopeReadablestream8,
  globalScopeWritablestream,
  globalScopeByteReadable,
  globalScopeTransformStream,
} from 'global-scope-streams';

export { default } from 'global-scope-streams';
