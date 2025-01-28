// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

import { default as async_hooks } from 'node-internal:async_hooks';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

export const { AsyncLocalStorage, AsyncResource } = async_hooks;

// We don't add all the async wrap providers since we don't use them
// and will not expose any APIs that use them.
export const asyncWrapProviders: Record<string, number> = {
  NONE: 0,
};

export function createHook(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('createHook');
}

export function executionAsyncId(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('executionAsyncId');
}

export function executionAsyncResource(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('executionAsyncResource');
}

export function triggerAsyncId(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('triggerAsyncId');
}

export default {
  AsyncLocalStorage,
  AsyncResource,
  asyncWrapProviders,
  createHook,
  executionAsyncId,
  executionAsyncResource,
  triggerAsyncId,
};
