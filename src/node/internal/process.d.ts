// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export function getEnvObject(): Record<string, string>;
export function getBuiltinModule(id: string): object;
export function exitImpl(code: number): void;
export function getCwd(): string;
export function setCwd(path: string): void;
export const versions: Record<string, string>;
export const platform: string;
export function shouldThrowOnNotImplementedTlsOption(): boolean;

export interface MemoryUsage {
  // Always 0: Resident Set Size is a process-wide OS metric and does not apply
  // to an individual Worker isolate in a multi-tenant process.
  rss: number;
  heapTotal: number;
  heapUsed: number;
  external: number;
  // Always 0: V8's default ArrayBuffer::Allocator does not report per-isolate
  // backing store byte totals via HeapStatistics.
  arrayBuffers: number;
}
export function getMemoryUsage(): MemoryUsage;

declare global {
  const Cloudflare: {
    readonly compatibilityFlags: Record<string, boolean> & {
      enable_streams_nodejs_v24_compat: boolean;
    };
  };
}

interface ErrorWithDetail extends Error {
  detail?: unknown;
}

interface EmitWarningOptions {
  type?: string | undefined;
  code?: string | undefined;
  ctor?: ErrorConstructor | undefined;
  detail?: string | undefined;
}
