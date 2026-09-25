// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the no-auto-allocate flag cell.

export {
  byobRequestIsNullWithoutAutoAllocate,
  byobRequestAvailableWithAutoAllocate,
  byobReadStillWorks,
  multipleReadsWithEnqueue,
  pipeToWorksWithoutAutoAllocate,
} from 'flag-no-auto-allocate';
