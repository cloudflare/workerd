// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

export * from 'node-internal:events';
export { default } from 'node-internal:events';

// Node-internal no-op hook used for async tracking bootstrapping. Exposed for
// feature-detection parity with Node.js; we do not track async resources here.
export function init(): void {}
