`opentelemetry-api-1.10.0.js` bundles the unmodified published
`@opentelemetry/api` 1.10.0 ES modules (Apache-2.0). Upstream source:
https://github.com/open-telemetry/opentelemetry-js/tree/12a5817ad147c186a0e45a6637abd5abf423727a/api

The published JavaScript omits source license headers. The bundle includes upstream attribution
and is covered by the Apache-2.0 license in the repository's root `LICENSE` file (also the
upstream project's license). Its only local additions are the attribution and targeted ESLint
disable prelude; upstream code is otherwise unchanged.
It tests registration, context propagation, overloads and native interoperation through
the real API rather than accessing the shared global directly.

To regenerate from an installation of that exact package:

```sh
esbuild node_modules/@opentelemetry/api/dist/index.mjs --bundle --format=esm \
  --legal-comments=inline --outfile=opentelemetry-api-1.10.0.js
```

Restore the attribution and ESLint prelude after regenerating. Bundled with esbuild 0.28.2.

Published tarball integrity (SHA-512 SRI):

- `https://registry.npmjs.org/@opentelemetry/api/-/api-1.10.0.tgz`:
  `sha512-lMkyes8iHrG+mk6WH7X70MdCLI2RS0FwNBY1PyQ8UemaijE2m5mv5A0Niy3HtbYTFrgkUEQjFMXmSAk2hQDuPQ==`
- `https://registry.npmjs.org/@opentelemetry/api/-/api-1.9.0.tgz`:
  `sha512-3giAOQvZiH5F9bMlMiv8+GSPMeqg0dbaeo58/0SlA9sxSqZhnUtxzX9/2FzyhS9sWQf5S0GJE0AKBrFqjpeYcg==`

`opentelemetry-api-1.9.0.js` bundles the published 1.9.0 `build/esm/index.js` entry
using the same esbuild command and attribution prelude. Both fixtures run the real-package
tests in separate workers. The runtime registers as 1.10.0: older compatible API clients can
consume that registration, but cannot replace it with another global provider.
