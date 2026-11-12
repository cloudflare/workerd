// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';
import { createHierarchyAwareCollector } from 'instrumentation-test-helper';

const collector = createHierarchyAwareCollector();
export default { tailStream: collector.tailStream };

export const validate = {
  async test() {
    await collector.waitForCompletion();
    const spans = [...collector.state.spans.values()].filter(
      ({ name }) => name === 'fetch'
    );
    assert.deepEqual(
      spans.map((span) => span['url.full']).sort(),
      [
        '/relative?q=1#f',
        'http://origin/start?hop=1',
        'http://origin/middle?hop=2#middle-fragment',
        'http://other/final?hop=3#final-fragment',
        'http://origin/x#f',
      ].sort()
    );
    assert.ok(spans.every(({ closed }) => closed));
  },
};
