// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The errors Worker Loader throws for code of the wrong type, the order it checks the code in,
// and a Worker whose modules C++ keeps in buffers that grow while they are added.

import assert from 'node:assert';

// Minimal valid WASM module that exports an add(i32, i32) -> i32 function.
const WASM_ADD_BYTES = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60, 0x02,
  0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01, 0x03, 0x61,
  0x64, 0x64, 0x00, 0x00, 0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01,
  0x6a, 0x0b,
]);

const MAIN = 'export default { fetch() { return new Response("ok"); } }';

function code(overrides) {
  return {
    compatibilityDate: '2025-01-01',
    mainModule: 'main.js',
    modules: { 'main.js': MAIN },
    ...overrides,
  };
}

function field(name, struct, type) {
  return (
    `Incorrect type for the '${name}' field on '${struct}': ` +
    `the provided value is not of type '${type}'.`
  );
}

const DATA = field('data', 'Module', 'Array');
const WASM = field('wasm', 'Module', 'Array or WasmModuleObject');
const ELEMENT_0 =
  "Incorrect type for array element 0: the provided value is not of type 'Fetcher'.";

function rejects(env, overrides, message) {
  assert.throws(() => env.loader.load(code(overrides)), {
    name: 'TypeError',
    message,
  });
}

// Each field whose conversion can fail, given a value of the wrong type, throws what C++ JSG throws
// for that field of the C++ struct.
export let wrongTypes = {
  test(ctrl, env) {
    rejects(env, { modules: { 'main.js': MAIN, 'a.bin': { data: 5 } } }, DATA);
    rejects(env, { modules: { 'main.js': MAIN, 'a.wasm': { wasm: 5 } } }, WASM);
    rejects(env, { env: 5 }, field('env', 'WorkerCode', 'Object'));
    rejects(env, { env: null }, field('env', 'WorkerCode', 'Object'));
    rejects(env, { tails: [5] }, ELEMENT_0);
    rejects(env, { tails: 5 }, field('tails', 'WorkerCode', 'Array'));
    rejects(env, { allowExperimental: true, streamingTails: [5] }, ELEMENT_0);
    rejects(
      env,
      { allowExperimental: true, streamingTails: 5 },
      field('streamingTails', 'WorkerCode', 'Array')
    );
    const worker = env.loader.load(code({}));
    const props = field('props', 'EntrypointOptions', 'Object');
    assert.throws(() => worker.getEntrypoint(undefined, { props: 5 }), {
      name: 'TypeError',
      message: props,
    });
    assert.throws(() => worker.getDurableObjectClass(undefined, { props: 5 }), {
      name: 'TypeError',
      message: props,
    });
  },
};

// A `globalOutbound` that isn't a `Fetcher` converts, as C++ JSG converts a
// `kj::Maybe<jsg::Ref<Fetcher>>`: under `dont_substitute_null_on_type_error` it is a `TypeError`,
// and otherwise it is none, which blocks the global outbound, as `null` does.
export let globalOutboundOfWrongType = {
  async test(ctrl, env) {
    const fetcher = code({
      globalOutbound: 5,
      modules: {
        'main.js': `export default {
          async fetch() {
            try {
              await fetch("http://example.com/");
              return new Response("fetched");
            } catch (e) {
              return new Response(e.message);
            }
          }
        }`,
      },
    });
    if (env.noSubstituteNull) {
      assert.throws(() => env.loader.load(fetcher), {
        name: 'TypeError',
        message: field('globalOutbound', 'WorkerCode', 'Fetcher'),
      });
      return;
    }
    const worker = env.loader.load(fetcher);
    const response = await worker.getEntrypoint().fetch('http://x/');
    assert.strictEqual(
      await response.text(),
      'This worker is not permitted to access the internet via global functions like fetch(). ' +
        "It must use capabilities (such as bindings in 'env') to talk to the outside world."
    );
  },
};

// Fields of the wrong type are found before any module is checked, as C++ JSG converts the whole
// struct first.
export let typeErrorsComeFirst = {
  test(ctrl, env) {
    // Not the error for a TypeScript module name.
    rejects(
      env,
      { modules: { 'x.ts': 'export {}', 'main.js': { data: 5 } } },
      DATA
    );
    // Not the error for a module object with two properties.
    rejects(env, { modules: { 'main.js': { js: MAIN, data: 5 } } }, DATA);
    // Not the error for `env`, which follows `modules` in the struct.
    rejects(env, { modules: { 'main.js': { data: 5 } }, env: 5 }, DATA);
    // Not the error for a Python module in a JavaScript Worker.
    rejects(
      env,
      { modules: { 'main.js': MAIN, 'x.py': '', 'w.wasm': { wasm: 5 } } },
      WASM
    );
  },
};

// A byte module and a WASM module, each before many text modules, so that the buffers C++ keeps
// the modules' names and bodies in grow while modules are added.
export let manyModules = {
  async test(ctrl, env) {
    const modules = {};
    modules['a.bin'] = { data: new TextEncoder().encode('bytes') };
    for (let i = 0; i < 20; i++) modules[`t${i}.js`] = `export default ${i};`;
    modules['w.wasm'] = { wasm: WASM_ADD_BYTES };
    for (let i = 20; i < 40; i++) modules[`t${i}.js`] = `export default ${i};`;
    let imports = '';
    let sum = '';
    for (let i = 0; i < 40; i++) {
      imports += `import t${i} from "t${i}.js";\n`;
      sum += `+ t${i}`;
    }
    modules['main.js'] = `
      ${imports}
      import bytes from "a.bin";
      import wasm from "w.wasm";
      export default {
        fetch() {
          const add = new WebAssembly.Instance(wasm).exports.add;
          const text = new TextDecoder().decode(bytes);
          return new Response(text + " " + add(0 ${sum}, 1));
        }
      }`;
    const worker = env.loader.load(code({ modules }));
    const response = await worker.getEntrypoint().fetch('http://x/');
    assert.strictEqual(await response.text(), 'bytes 781');
  },
};
