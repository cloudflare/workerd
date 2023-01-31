// Type definitions for c++ implementation.

export interface AsyncResourceOptions {
  triggerAsyncId?: number;
}

export class AsyncResource {
  public constructor(type: string, options?: AsyncResourceOptions);
  public runInAsyncScope<R>(fn: (...args: unknown[]) => R, ...args: unknown[]): R;

  public bind<Func extends (...args: unknown[]) => unknown>(
    fn: Func): Func & { asyncResource: AsyncResource; };

  public static bind<Func extends (this: ThisArg, ...args: unknown[]) => unknown, ThisArg>(
    fn: Func, type?: string, thisArg?: ThisArg): Func & { asyncResource: AsyncResource; };
}

export class AsyncLocalStorage<T> {
  public run<R>(store: T, fn: (...args: unknown[]) => R, ...args: unknown[]): R;
  public exit<R>(fn: (...args: unknown[]) => R, ...args: unknown[]): R;
  public getStore(): T;
}
