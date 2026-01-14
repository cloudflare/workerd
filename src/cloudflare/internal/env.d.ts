// Get the current environment, if any
export function getCurrentEnv(): Record<string, unknown> | undefined
export function getCurrentExports(): Record<string, unknown> | undefined
export function withEnv(newEnv: unknown, fn: () => unknown): unknown
export function withExports(newExports: unknown, fn: () => unknown): unknown
export function withEnvAndExports(
  newEnv: unknown,
  newExports: unknown,
  fn: () => unknown,
): unknown
