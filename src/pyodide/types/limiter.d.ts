// A typescript declaration file for the internal Limiter JSG class
// see src/workerd/api/pyodide/pyodide.h (SimplePythonLimiter)

import ArtifactBundler from './artifacts.js';

interface Limiter {
  beginStartup: () => void;
  finishStartup: (
    snapshotType: ArtifactBundler.SnapshotType | undefined
  ) => void;
}

declare const limiter: Limiter;

export default limiter;
