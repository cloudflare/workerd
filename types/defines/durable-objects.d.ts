declare module 'cloudflare:durable-objects' {
  /**
   * Marks Durable Object methods as safe to run more than once. The runtime may then retry calls
   * to them after a disconnect that might have happened after the method started. It does not
   * enable retries or change how many are made.
   *
   * Mark methods on the class prototype, for example from a static block:
   *
   * ```ts
   * class Counter extends DurableObject {
   *   static {
   *     retryable(this.prototype.reset);
   *   }
   *   async reset() {}
   * }
   * ```
   *
   * The mark is on the function itself. A subclass inherits it, but an override, a bound copy, or
   * a wrapper is not marked unless it is also passed to `retryable()`.
   *
   * Each attempt creates its own return value, so if the method returns an `RpcTarget`, its
   * `[Symbol.dispose]()` may run once per attempt.
   *
   * @throws TypeError if an argument is not a function.
   */
  export function retryable(
    ...methods: ((...args: never[]) => unknown)[]
  ): void;
}
