// Type definitions for c++ implementation.

export interface AsyncResourceOptions {
  triggerAsyncId?: number
}

export class AsyncResource {
  constructor(type: string, options?: AsyncResourceOptions)
  runInAsyncScope<R>(fn: (...args: unknown[]) => R, ...args: unknown[]): R

  bind<Func extends (...args: unknown[]) => unknown>(
    fn: Func,
  ): Func & { asyncResource: AsyncResource }

  static bind<
    Func extends (this: ThisArg, ...args: unknown[]) => unknown,
    ThisArg,
  >(
    fn: Func,
    type?: string,
    thisArg?: ThisArg,
  ): Func & { asyncResource: AsyncResource }
}

export class AsyncLocalStorage<T> {
  run<R>(store: T, fn: (...args: unknown[]) => R, ...args: unknown[]): R
  exit<R>(fn: (...args: unknown[]) => R, ...args: unknown[]): R
  getStore(): T
}
