// Test that verifies compat flags are at NEWEST state (all enabled).
// This test only runs in the @all-compat-flags variant (default variant is disabled).

import { strictEqual } from 'node:assert';

export const compatFlagEnabledTest = {
  test() {
    // With compat date 2999-12-31, formdata_parser_supports_files should be enabled
    // (it was enabled on 2021-11-03).
    const isEnabled =
      Cloudflare.compatibilityFlags.formdata_parser_supports_files;
    strictEqual(
      isEnabled,
      true,
      'formdata_parser_supports_files should be ENABLED with newest compat date'
    );
  },
};
