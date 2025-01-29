// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

import { default as async_hooks } from 'node-internal:async_hooks';

class AsyncHook {
  public enable(): this {
    return this;
  }

  public disable(): this {
    return this;
  }
}

export const { AsyncLocalStorage, AsyncResource } = async_hooks;

// We don't add all the async wrap providers since we don't use them
// and will not expose any APIs that use them.
export const asyncWrapProviders: Record<string, number> = {
  NONE: 0,
};

export function createHook(): AsyncHook {
  // Even though we don't implement this function, we return a default value
  // in order to preserve backward compatibility and avoid breaking changes
  // with unenv polyfills.
  return new AsyncHook();
}

export function executionAsyncId(): number {
  // Even though we don't implement this function, we return a default value
  // in order to preserve backward compatibility and avoid breaking changes
  // with unenv polyfills.
  return 0;
}

export function executionAsyncResource(): Record<string, string> {
  // Even though we don't implement this function, we return a default value
  // in order to preserve backward compatibility and avoid breaking changes
  // with unenv polyfills.
  // eslint-disable-next-line @typescript-eslint/no-unsafe-return
  return Object.create(null);
}

export function triggerAsyncId(): number {
  // Even though we don't implement this function, we return a default value
  // in order to preserve backward compatibility and avoid breaking changes
  // with unenv polyfills.
  return 0;
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
