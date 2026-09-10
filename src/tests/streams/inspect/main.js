// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the inspect suite. Explicit named re-exports only.

export {
  inspectValueReadable,
  inspectErroredReadable,
  inspectByteReadable,
  inspectWritable,
  inspectErroringWritable,
  inspectFixedLengthStream,
  inspectErroredIdentityStream,
} from 'inspect';
