// A typescript declaration file for the internal WorkerFatalReporter JSG class
// see src/workerd/api/pyodide/pyodide.h (WorkerFatalReporter)

interface FatalReporter {
  reportFatal: () => void;
}

declare const fatalReporter: FatalReporter;

export default fatalReporter;
