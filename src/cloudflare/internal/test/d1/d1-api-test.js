// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { testD1ApiQueriesHappyPath, testD1Exec } from './d1-api-test-common';

export const testWithoutSessions = {
  async test(_ctr, env) {
    await testD1ApiQueriesHappyPath(env.d1);
  },
};

export const testExec = {
  async test(_ctr, env) {
    await testD1Exec(env.d1);
  },
};
