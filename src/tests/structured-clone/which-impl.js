// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Which structuredClone implementation this cell runs, so tests can pin each
// side of a deliberate divergence exactly. The implementation is selected by
// the TS_STRUCTURED_CLONE autogate, which user code cannot read, so each cell
// says which one it is meant to run through its USING_TS_IMPL binding.
import { env } from 'cloudflare:workers';

export const usingTsImpl = env.USING_TS_IMPL === true;
