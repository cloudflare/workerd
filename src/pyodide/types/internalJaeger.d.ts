// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare namespace internalJaeger {
  const traceId: number | null,
    enterSpan: <T>(name: string, callback: () => T) => T;
}

export default internalJaeger;
