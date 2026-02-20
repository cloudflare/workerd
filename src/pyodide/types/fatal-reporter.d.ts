// A typescript declaration file for the internal WorkerFatalReporter JSG class
// see src/workerd/api/pyodide/pyodide.h (WorkerFatalReporter)

interface FatalReporter {
  reportFatal: (error: string) => void;
}

declare const fatalReporter: FatalReporter;

export default fatalReporter;
