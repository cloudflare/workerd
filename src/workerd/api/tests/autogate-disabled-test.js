// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Test that verifies autogates are DISABLED.
// This test runs in the default variant, where WORKERD_ALL_AUTOGATES is not
// set, and in the @all-autogates variant, which leaves TEST_WORKERD out
// (all_autogates_variant_excludes in BUILD.bazel).

import { strictEqual } from 'node:assert';
import unsafe from 'workerd:unsafe';

export const autogateDisabledTest = {
  test() {
    const isEnabled = unsafe.isTestAutogateEnabled();
    strictEqual(isEnabled, false, 'TEST_WORKERD autogate should be DISABLED');
  },
};
