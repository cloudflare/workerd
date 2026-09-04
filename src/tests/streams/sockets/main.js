// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the sockets streams suite. Explicit named re-exports
// only.

export {
  echoRoundTrip,
  greetReadsToEof,
  echoByobReads,
  echoReadAtLeast,
  pipeSocketReadableToJsSink,
  pipeJsSourceToSocketWritable,
  pipeSocketThroughJsTransform,
  pipeSocketToSocket,
  cancelReadableSettlesSocket,
  largeEchoVolume,
} from 'socket-streams';
