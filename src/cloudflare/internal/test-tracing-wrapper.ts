// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Test-only wrapper module for internal tracing utilities
// This module is ONLY for testing and should not be used in production code

import { withSpan } from 'cloudflare-internal:tracing-helpers';
import tracing from 'cloudflare-internal:tracing';

interface TestWrapper {
  withSpan: typeof withSpan;
  tracing: typeof tracing;
  createMockSpan: (name: string) => ReturnType<typeof tracing.startSpan>;
}

// Wrapper function that provides test utilities for tracing
export default function (_env: unknown): TestWrapper {
  return {
    // Export withSpan for testing
    withSpan,

    // Also export the raw tracing module for tests that need it
    tracing,

    // Test helper: create a mock span for testing
    createMockSpan(name: string): ReturnType<typeof tracing.startSpan> {
      return tracing.startSpan(name);
    },
  };
}
