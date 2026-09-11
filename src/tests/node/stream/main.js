// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the node:stream web-interop suite. Explicit named
// re-exports only: a name collision between two modules must be a load-time
// SyntaxError, never a silently dropped test.

export { streamWebReexportsGlobals, adapterEntryPoints } from 'api-surface';

export { toWebDeliversPushedChunk } from 'readable-to-web';

export {
  fromWebDeliversDataEvents,
  fromWebErroredAtStartRejectsAsyncIteration,
  fromWebPullErrorRejectsAsyncIteration,
} from 'readable-from-web';

export {
  toWebWritesReachNodeSink,
  toWebCloseEndsNodeWritable,
  toWebPipeToCompletes,
  toWebSyncNodeErrorRejectsPendingWrite,
  toWebAsyncNodeErrorErrorsStream,
  toWebFinalErrorRejectsClose,
  toWebNodeEndWithoutCloseAbortsStream,
  toWebNodeDestroyBecomesAbortError,
  toWebNodeDestroyWithErrorErrorsStream,
} from 'writable-to-web';

export { fromWebWritesReachWebSink } from 'writable-from-web';

export { toWebPairRoundTrip } from 'duplex-to-web';

export {
  fromWebPairRoundTrip,
  fromWebObjectModeStrings,
} from 'duplex-from-web';

export { toWebAsResponseBody } from 'bodies';
