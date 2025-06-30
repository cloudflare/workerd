export const versions: Record<string, string>;
export function getEnvObject(): Record<string, string>;
export function getBuiltinModule(id: string): object;
export function exitImpl(code: number): void;
export const platform: string;
export const nodeVersion: string;
export const workerdVersion: string;

interface EmitWarningOptions {
  type?: string | undefined;
  code?: string | undefined;
  ctor?: ErrorConstructor | undefined;
  detail?: string | undefined;
}
