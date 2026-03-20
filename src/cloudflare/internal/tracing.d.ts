// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export class Span {
  // Sets an attribute on this span. If value is undefined, the attribute is not set.
  setAttribute(key: string, value: string | number | boolean | undefined): void;
  // Closes the span
  end(): void;
}

function startSpan(name: string): Span;
