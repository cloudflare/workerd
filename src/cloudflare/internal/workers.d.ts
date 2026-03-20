// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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

export class RpcPromise {}

export class RpcProperty {}

export class RpcTarget {}

export class ServiceStub {}

export function waitUntil(promise: Promise<unknown>): void;
