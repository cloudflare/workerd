// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import entrypoints from 'cloudflare-internal:workers';

export function retryable<This, Args extends unknown[], Return>(
  value: (this: This, ...args: Args) => Return,
  context: ClassMethodDecoratorContext<
    This,
    (this: This, ...args: Args) => Return
  >
): (this: This, ...args: Args) => Return {
  const method = entrypoints.retryable(value, context);
  // A decorator applied after this one may install a wrapper that lacks the marker. The method then
  // behaves as if undecorated, so warn instead of failing. Instance-method initializers run in the
  // constructor, after the class has installed its methods.
  let checked = false;
  context.addInitializer(function (this: This): void {
    if (checked) return;
    checked = true;
    try {
      // Look for the marked function on the class that declares the method, which may be a parent
      // of the instance's class. It is absent if a later decorator installed a replacement.
      for (
        let prototype: unknown = Object.getPrototypeOf(this);
        prototype !== null;
        prototype = Object.getPrototypeOf(prototype)
      ) {
        const descriptor = Object.getOwnPropertyDescriptor(
          prototype,
          context.name
        );
        if (descriptor?.value === method) return;
      }
    } catch {
      // A Proxy on the prototype chain threw. The check is only a diagnostic, so it must not fail
      // construction.
      return;
    }
    console.warn(
      `@retryable on method "${String(context.name)}" has no effect because a decorator ` +
        'applied after it replaced the method. Apply @retryable outermost (first in source ' +
        'order).'
    );
  });
  return method;
}
