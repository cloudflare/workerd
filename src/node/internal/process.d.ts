export function getEnvObject(): Record<string, string>;
export function getBuiltinModule(id: string): object;
export function exitImpl(code: number): void;
export function writeAndFlush(fd: number, buffer: Uint8Array): void;
export const versions: Record<string, string>;
export const platform: string;

declare global {
  const Cloudflare: {
    readonly compatibilityFlags: Record<string, boolean>;
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
