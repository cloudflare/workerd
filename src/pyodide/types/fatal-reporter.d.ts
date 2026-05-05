// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A typescript declaration file for the internal WorkerFatalReporter JSG class
// see src/workerd/api/pyodide/pyodide.h (WorkerFatalReporter)

interface FatalReporter {
  reportFatal: (error: string) => void;
  reportPythonWorkersInternalError: () => void;
}

declare const fatalReporter: FatalReporter;

export default fatalReporter;
