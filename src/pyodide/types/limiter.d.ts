// A typescript declaration file for the internal Limiter JSG class
// see src/workerd/api/pyodide/pyodide.h (SimplePythonLimiter)

interface Limiter {
  beginStartup: () => void;
  finishStartup: () => void;
}

declare const limiter: Limiter;

export default limiter;
