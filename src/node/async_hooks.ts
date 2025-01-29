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

  // IMPORTANT
  //
  // The following methods were defined in unenv while they don't exist in Node.js
  // async_hooks module. This is a bug with unenv.
  // Unfortunately, unless we are 100% sure these methods are not used in production,
  // we can not remove them from workerd.
  //
  // More information about the bug is available at https://github.com/unjs/unenv/issues/403
  //
  // Ref: https://github.com/unjs/unenv/blob/ada7c4093c69c9729f1b008a8ab7902389941624/src/runtime/node/async_hooks/internal/async-hook.ts#L25
  public init(): void {}
  public before(): void {}
  public after(): void {}
  public destroy(): void {}
  public promiseResolve(): void {}
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
