import { default as internalJaeger } from "pyodide-internal:internalJaeger";

/**
 * Used for tracing via Jaeger.
 */
export function enterJaegerSpan(span: String, callback: Function) {
  if (!internalJaeger.traceId) {
    // Jaeger tracing not enabled or traceId is not present in request.
    return callback();
  }

  return internalJaeger.enterSpan(span, callback);
}
