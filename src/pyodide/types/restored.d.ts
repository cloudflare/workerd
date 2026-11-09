// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The `pyodide` object of a Pyodide restored from a V8 heap snapshot into a context of its own
// in this isolate, or undefined when Pyodide is to be loaded from scratch. See
// src/workerd/api/pyodide/snapshot-harness.h.
declare const RestoredPyodide: Pyodide | undefined;

export default RestoredPyodide;
