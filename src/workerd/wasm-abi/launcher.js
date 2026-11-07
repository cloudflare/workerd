// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The main module of a Wasm-only worker: it serves the bundle's `worker.wasm` through the
// Workers Wasm ABI. A native host, if the runtime has one that implements every Wasm import
// module the guest uses, serves it; otherwise the shim does, with one shim module per import
// module. See "Launcher" in conventions.md.
import wasm from './worker.wasm';

// The default export of the shim module of each Wasm import module the guest uses:
// `cf:<name>/v<n>` is served by `./cf-<name>-v<n>.js`.
function loadShims() {
  const names = new Set(WebAssembly.Module.imports(wasm).map((i) => i.module));
  return Promise.all(
    [...names].map(async (name) => {
      const m = /^cf:([a-z][a-z0-9]*)\/v([1-9][0-9]*)$/.exec(name);
      if (m === null) {
        throw new TypeError(
          `Wasm import module ${name} is not a module of the Workers Wasm ABI`
        );
      }
      const specifier = `./cf-${m[1]}-v${m[2]}.js`;
      try {
        return (await import(specifier)).default;
      } catch (cause) {
        throw new Error(
          `cannot load ${specifier}, the shim module of Wasm import module ${name}`,
          { cause }
        );
      }
    })
  );
}

async function host() {
  const native = await import('workerd:wasm-abi-native').then(
    (m) => m.default,
    () => undefined
  );
  const served = native?.serve(wasm);
  if (served !== undefined) return served;
  const [runtime, shims] = await Promise.all([
    import('./runtime.js'),
    loadShims(),
  ]);
  return runtime.serve(wasm, shims);
}

export default await host();
