// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import entrypoints from 'cloudflare-internal:workers';

export function retryable(...methods: ((...args: never[]) => unknown)[]): void {
  entrypoints.retryable(...methods);
}
