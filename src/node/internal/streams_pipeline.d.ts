// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export { pipeline } from 'node:stream';

export function pipelineImpl(
  streams: unknown,
  callback: (err: Error | null, value?: unknown) => void,
  opts: { signal?: AbortSignal | undefined; end?: boolean | undefined }
): unknown;
