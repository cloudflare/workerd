declare module 'cloudflare:durable-objects' {
  export { DurableObject } from 'cloudflare:workers';

  /**
   * Marks a Durable Object method as safe to run more than once. The runtime may then retry calls
   * to it after a disconnect that might have happened after the method started. It does not
   * enable retries or change how many are made.
   *
   * A retry repeats only the call to this method. Calls on the value it returns are never
   * retried. Anything the method creates may be created once per attempt, so its cleanup may also
   * run once per attempt. For example, `[Symbol.dispose]()` runs on each `RpcTarget` it returned.
   *
   * Only public instance methods can be decorated. When composing decorators, apply `@retryable`
   * outermost (first in source order) so it marks the method that is finally installed.
   * Requires standard (not `experimentalDecorators`) decorators and a bundler that transforms
   * them, such as Wrangler.
   */
  export function retryable<This, Args extends unknown[], Return>(
    value: (this: This, ...args: Args) => Return,
    context: ClassMethodDecoratorContext<
      This,
      (this: This, ...args: Args) => Return
    >
  ): (this: This, ...args: Args) => Return;
}
