// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Type definitions for c++ implementation.

export class EmailMessage {
  public constructor(from: string, to: string, raw: ReadableStream | string);
  public readonly from: string;
  public readonly to: string;
}
