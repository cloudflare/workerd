import { default as internalJaeger } from "pyodide-internal:internalJaeger";

/**
 * Used for tracing via Jaeger.
 */
export function enterJaegerSpan<T>(span: string, callback: () => T): T {
  if (!internalJaeger.traceId) {
    // Jaeger tracing not enabled or traceId is not present in request.
    return callback();
  }

  return internalJaeger.enterSpan(span, callback);
}
