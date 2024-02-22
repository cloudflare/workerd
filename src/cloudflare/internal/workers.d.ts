export class DurableObject {
  public constructor(ctx: unknown, env: unknown);

  public ctx: unknown;
  public env: unknown;
}

export class WorkerEntrypoint {
  public constructor(ctx: unknown, env: unknown);

  public ctx: unknown;
  public env: unknown;
}

export class RpcStub {
  public constructor(server: object);
}

export class RpcTarget {
}
