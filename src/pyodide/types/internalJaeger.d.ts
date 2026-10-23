// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare namespace internalJaeger {
  interface InternalSpan {
    setTags(tags: Record<string, string | number | boolean>): void;
  }

  const traceId: number | null,
    enterSpan: <T>(name: string, callback: (span: InternalSpan) => T) => T;
}

export default internalJaeger;
