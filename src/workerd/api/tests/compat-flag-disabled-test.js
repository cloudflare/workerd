// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Test that verifies compat flags are at OLDEST state (most disabled).
// This test only runs in the default @ variant (@all-compat-flags variant is disabled).

import { strictEqual } from 'node:assert';

export const compatFlagDisabledTest = {
  test() {
    // With compat date 2000-01-01, formdata_parser_supports_files should be disabled
    // (it was enabled on 2021-11-03).
    const isEnabled =
      Cloudflare.compatibilityFlags.formdata_parser_supports_files;
    strictEqual(
      isEnabled,
      false,
      'formdata_parser_supports_files should be DISABLED with oldest compat date'
    );
  },
};
