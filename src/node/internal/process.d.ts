export function getEnvObject(): Record<string, string>;
export function getBuiltinModule(id: string): object;
export function exitImpl(code: number): void;
export function getCwd(): string;
export function setCwd(path: string): void;
export const versions: Record<string, string>;
export const platform: string;

interface ErrorWithDetail extends Error {
  detail?: unknown;
}

interface EmitWarningOptions {
  type?: string | undefined;
  code?: string | undefined;
  ctor?: ErrorConstructor | undefined;
  detail?: string | undefined;
}
