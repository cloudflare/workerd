// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Test that verifies autogates are ENABLED.
// This test only runs in the @all-autogates variant (default variant is disabled).

import { strictEqual } from 'node:assert';
import unsafe from 'workerd:unsafe';

export const autogateEnabledTest = {
  test() {
    // In the @all-autogates variant, WORKERD_ALL_AUTOGATES env var is set,
    // so isTestAutogateEnabled() should return true.
    const isEnabled = unsafe.isTestAutogateEnabled();
    strictEqual(
      isEnabled,
      true,
      'TEST_WORKERD autogate should be ENABLED in @all-autogates variant'
    );
  },
};
