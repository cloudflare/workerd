// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TEST-ONLY MODULE - DO NOT USE IN PRODUCTION
// This wrapper exists solely to expose internal tracing utilities for testing.
// It must be in the internal/ directory to be compiled as part of the cloudflare bundle,
// but it should never be used outside of test configurations.

import { withSpan } from 'cloudflare-internal:tracing-helpers';

interface TestWrapper {
  withSpan: typeof withSpan;
}

// Wrapper function that provides test utilities for tracing
export default function (_env: unknown): TestWrapper {
  return {
    // Export withSpan for testing
    withSpan,
  };
}
