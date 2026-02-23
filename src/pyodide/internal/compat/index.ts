/**
 * Version adapter registry.
 *
 * Provides getPyodideVersionAdapter() to access the adapter for the current Pyodide version.
 *
 * When adding a new Pyodide version:
 * 1. Create a new file pyodide-X.Y.Z.ts implementing PyodideVersionAdapter
 * 2. Import and register it in the `adapters` map below
 * 3. Update the API.version union type in types/emscripten.d.ts
 */
import { PythonWorkersInternalError } from 'pyodide-internal:util';
import { adapter as v026 } from 'pyodide-internal:compat/pyodide-0.26.0a2';
import { adapter as v028 } from 'pyodide-internal:compat/pyodide-0.28.2';
import type { PyodideVersionAdapter } from 'pyodide-internal:compat/compat-api';

export type { PyodideVersionAdapter } from 'pyodide-internal:compat/compat-api';

const adapters: Record<string, PyodideVersionAdapter> = {
  '0.26.0a2': v026,
  '0.28.2': v028,
};

let _adapter: PyodideVersionAdapter | undefined;

/**
 * Get the version adapter for the current Pyodide version.
 */
export function getPyodideVersionAdapter(
  Module?: Module
): PyodideVersionAdapter {
  if (!_adapter) {
    if (!Module) {
      throw new PythonWorkersInternalError(
        'Version adapter not initialized. Pass the Module object to getPyodideVersionAdapter() on first call.'
      );
    }
    const version = Module.API.version;
    const adapter = adapters[version];
    if (!adapter) {
      throw new PythonWorkersInternalError(
        `Unsupported Pyodide version: ${version}. ` +
          `Supported versions: ${Object.keys(adapters).join(', ')}`
      );
    }
    _adapter = adapter;
  }
  return _adapter;
}
