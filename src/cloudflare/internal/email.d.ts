// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Type definitions for c++ implementation.

export class EmailMessage {
  constructor(from: string, to: string, raw: ReadableStream | string)
  readonly from: string
  readonly to: string
}
