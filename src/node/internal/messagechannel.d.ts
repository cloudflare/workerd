// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Type definitions for cloudflare-internal:messagechannel
// This internal module exposes MessageChannel and MessagePort for use by
// built-in modules like node:worker_threads, independent of the
// expose_global_message_channel compatibility flag.

declare namespace _default {
  const MessageChannel: typeof globalThis.MessageChannel;
  const MessagePort: typeof globalThis.MessagePort;
}
export default _default;
