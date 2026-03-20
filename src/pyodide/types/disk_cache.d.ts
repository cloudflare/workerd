// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare namespace DiskCache {
  const get: (key: string) => ArrayBuffer | null;
  const put: (key: string, val: ArrayBuffer | Uint8Array) => void;
  const putSnapshot: (key: string, val: ArrayBuffer | Uint8Array) => void;
}

export default DiskCache;
