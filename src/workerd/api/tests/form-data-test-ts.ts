// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

// https://github.com/cloudflare/workerd/issues/5934
export const formDataUnionTypeOverloads = {
  test() {
    const formData = new FormData();

    formData.append('stringKey', 'stringValue' as string | Blob);
    strictEqual(formData.get('stringKey'), 'stringValue');

    // We explicitly use string | Blob to test types.
    const blob: string | Blob = new Blob(['blob content'], {
      type: 'text/plain',
    });
    formData.append('blobKey', blob);
    strictEqual(formData.get('blobKey') instanceof File, true);

    const value: string | Blob = 'setValue';
    formData.set('setStringKey', value);
    strictEqual(formData.get('setStringKey'), 'setValue');

    formData.set('setBlobKey', blob);
    strictEqual(formData.get('setBlobKey') instanceof File, true);

    const values: Array<{ key: string; value: string | Blob }> = [
      { key: 'key1', value: 'string value' },
      { key: 'key2', value: new Blob(['blob']) },
    ];

    const formData2 = new FormData();
    for (const { key, value } of values) {
      if (typeof value === 'string' || value instanceof Blob) {
        formData2.append(key, value);
      }
    }

    strictEqual(formData2.get('key1'), 'string value');
    strictEqual(formData2.get('key2') instanceof File, true);
  },
};
