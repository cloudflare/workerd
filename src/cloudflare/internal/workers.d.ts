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

// Stubs for the C++ CacheContext types (see src/workerd/api/global-scope.h).
// The full interface is also generated in types/generated-snapshot/ from C++ RTTI.
// If you change the C++ types, update these to match.
export interface CachePurgeError {
  code: number;
  message: string;
}

export interface CachePurgeResult {
  success: boolean;
  errors: CachePurgeError[];
}

export interface CachePurgeOptions {
  tags?: string[];
  pathPrefixes?: string[];
  purgeEverything?: boolean;
}

export interface CacheContext {
  purge(options: CachePurgeOptions): Promise<CachePurgeResult>;
}

export function getCtxCache(): CacheContext | undefined;

export function abortIsolate(reason?: string): never;

// True when the workerd_experimental compat flag is enabled. Use this for gating experimental
// re-exports in user-facing wrappers; Cloudflare.compatibilityFlags filters out experimental
// flags themselves so it cannot be used to detect this.
export const isExperimental: boolean;
