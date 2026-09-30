declare module 'cloudflare:durable-objects' {
  /**
   * Marks a Durable Object method as safe to run more than once. The runtime may then retry calls
   * to it after a disconnect that might have happened after the method started. It does not
   * enable retries or change how many are made.
   *
   * Each attempt creates its own return value, so if the method returns an `RpcTarget`, its
   * `[Symbol.dispose]()` may run once per attempt.
   *
   * Only public instance methods can be decorated. When composing decorators, apply `@retryable`
   * outermost (first in source order) so it marks the method that is finally installed.
   * Otherwise `@retryable` has no effect, and constructing the object logs a warning.
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
