// Test that verifies autogates are DISABLED.
// This test only runs in the default variant (all-autogates variant is disabled).

import { strictEqual } from 'node:assert';
import unsafe from 'workerd:unsafe';

export const autogateDisabledTest = {
  test() {
    // In the default variant, WORKERD_ALL_AUTOGATES env var is NOT set,
    // so isTestAutogateEnabled() should return false.
    const isEnabled = unsafe.isTestAutogateEnabled();
    strictEqual(
      isEnabled,
      false,
      'TEST_WORKERD autogate should be DISABLED in default variant'
    );
  },
};
