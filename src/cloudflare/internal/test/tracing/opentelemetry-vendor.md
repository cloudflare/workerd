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
