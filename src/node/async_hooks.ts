// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

import { default as async_hooks } from 'node-internal:async_hooks';

export const { AsyncLocalStorage, AsyncResource } = async_hooks;

export default {
  AsyncLocalStorage,
  AsyncResource,
};
