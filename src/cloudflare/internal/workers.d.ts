export class DurableObject {
  constructor(ctx: unknown, env: unknown);

  ctx: unknown;
  env: unknown;
}

export class WorkerEntrypoint {
  constructor(ctx: unknown, env: unknown);

  ctx: unknown;
  env: unknown;
}

export class WorkflowEntrypoint {
  constructor(ctx: unknown, env: unknown);

  ctx: unknown;
  env: unknown;
}

export class RpcStub {
  constructor(server: object);
}

export class RpcTarget {}

export function waitUntil(promise: Promise<unknown>): void;
export function registerRpcTargetClass(
  constructor: new (...args: any[]) => any
): void;
