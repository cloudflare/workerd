// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { MockTracker, MockFunctionContext } from 'node-internal:mock';

const mock = new MockTracker();

export { mock, MockFunctionContext, MockTracker };
export default {
  mock,
  MockFunctionContext,
  MockTracker,
};
