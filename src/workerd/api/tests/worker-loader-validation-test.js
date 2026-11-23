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

function respond(text) {
  return code({
    modules: {
      'main.js': `export default { fetch() { return new Response(${JSON.stringify(text)}); } }`,
    },
  });
}

async function fetchText(worker) {
  const response = await worker.getEntrypoint().fetch('http://x/');
  return response.text();
}

// A `SharedArrayBuffer`, or a view of one, is bytes for a `data` or `wasm` module, as C++ JSG
// takes a `kj::Array<const byte>`.
export let sharedArrayBuffers = {
  async test(ctrl, env) {
    const shared = (bytes) => {
      const buffer = new SharedArrayBuffer(bytes.byteLength);
      new Uint8Array(buffer).set(bytes);
      return buffer;
    };
    const text = new TextEncoder().encode('shared');
    const worker = env.loader.load(
      code({
        modules: {
          'main.js': `
            import bytes from "a.bin";
            import view from "b.bin";
            import wasm from "w.wasm";
            export default {
              fetch() {
                const add = new WebAssembly.Instance(wasm).exports.add;
                const decode = (b) => new TextDecoder().decode(b);
                return new Response(decode(bytes) + " " + decode(view) + " " + add(2, 3));
              }
            }`,
          'a.bin': { data: shared(text) },
          'b.bin': { data: new Uint8Array(shared(text), 1, 4) },
          'w.wasm': { wasm: shared(WASM_ADD_BYTES) },
        },
      })
    );
    assert.strictEqual(await fetchText(worker), 'shared hare 5');
  },
};

// `get()` takes a name, which may be `null` or `undefined` but not missing, as C++ JSG converts a
// `kj::Maybe<kj::String>` parameter.
export let getArguments = {
  async test(ctrl, env) {
    assert.throws(() => env.loader.get(), {
      name: 'TypeError',
      message:
        "Failed to execute 'get' on 'WorkerLoader': parameter 1 is not of type 'string'.",
    });
    assert.throws(() => env.loader.get(null), {
      name: 'TypeError',
      message:
        "Failed to execute 'get' on 'WorkerLoader': parameter 2 is not of type 'function'.",
    });
    const worker = env.loader.get(undefined, () => respond('unnamed'));
    assert.strictEqual(await fetchText(worker), 'unnamed');
  },
};

// `getCode()` is called with the global object as `this`, as C++ JSG calls a `jsg::Function`.
export let getCodeThis = {
  async test(ctrl, env) {
    let thisIsGlobal;
    const worker = env.loader.get('get-code-this', function () {
      thisIsGlobal = this === globalThis;
      return respond('called');
    });
    assert.strictEqual(await fetchText(worker), 'called');
    assert.strictEqual(thisIsGlobal, true);
  },
};

// What `getCode()` returns converts as C++ JSG converts a callback's `jsg::Promise<WorkerCode>`:
// a promise to what it resolves to, a thenable to what it resolves to only under
// `unwrap_custom_thenables`, and anything else to `WorkerCode` itself.
export let getCodeResults = {
  async test(ctrl, env) {
    assert.strictEqual(
      await fetchText(env.loader.get('plain', () => respond('plain'))),
      'plain'
    );
    assert.strictEqual(
      await fetchText(
        env.loader.get('promise', async () => respond('promise'))
      ),
      'promise'
    );

    const thenable = {
      ...respond('the thenable itself'),
      then(resolve) {
        resolve(respond('what it resolves to'));
      },
    };
    assert.strictEqual(
      await fetchText(env.loader.get('thenable', () => thenable)),
      env.unwrapThenables ? 'what it resolves to' : 'the thenable itself'
    );

    await assert.rejects(fetchText(env.loader.get('number', () => 5)), {
      name: 'TypeError',
      message: "Callback returned incorrect type; expected 'Promise'",
    });
    await assert.rejects(
      fetchText(env.loader.get('async-number', async () => 5)),
      {
        name: 'TypeError',
        message:
          "Incorrect type for Promise: the Promise did not resolve to 'WorkerCode'.",
      }
    );
  },
};

// A promise from `getCode()` is converted to `WorkerCode` in a reaction of its own, and the
// Worker's source, which serializes `env`, is made in the next one, as C++ JSG converts the
// promise before `IoContext::awaitJs()` reacts to it.
export let getCodeConversionOrder = {
  async test(ctrl, env) {
    const log = [];
    const worker = env.loader.get('conversion-order', () => {
      queueMicrotask(() => log.push('queued by getCode'));
      return Promise.resolve({
        ...respond('ordered'),
        get compatibilityDate() {
          log.push('convert');
          queueMicrotask(() => log.push('queued by conversion'));
          return '2025-01-01';
        },
        env: {
          get value() {
            log.push('serialize env');
            return 1;
          },
        },
      });
    });
    assert.strictEqual(await fetchText(worker), 'ordered');
    assert.deepStrictEqual(log, [
      'queued by getCode',
      'convert',
      'queued by conversion',
      'serialize env',
    ]);
  },
};
