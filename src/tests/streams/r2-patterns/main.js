// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the r2-patterns suite. Explicit named re-exports only.

export {
  byobReadAtLeastAutomatic,
  byobReadAtLeastManual,
  identityTransformReadAtLeast,
  fixedLengthStreamReadAtLeast,
  closedByobTeeOnStart,
  identityTransformStreamReadAtLeast,
  partiallyFilledByobAtLeast,
  byobReadAtLeastTee,
  byobReadAtLeastTeeComplex1,
  byobReadAtLeastTeeComplex2,
  byobReadAtLeastTeeComplex3,
  requestCloneByob,
  textDecoderStreamRequest,
} from 'r2-consumption';
