// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
