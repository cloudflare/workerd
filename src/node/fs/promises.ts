// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as fs from 'node-internal:internal_fs_promises';
import * as constants from 'node-internal:internal_fs_constants';

export * from 'node-internal:internal_fs_promises';
export { constants };

export default {
  constants,
  ...fs,
};
