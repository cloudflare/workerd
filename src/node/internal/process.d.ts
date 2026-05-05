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
