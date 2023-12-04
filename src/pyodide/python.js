import { _createPyodideModule } from "pyodide-internal:pyodide-bundle/pyodide.asm";
import module from "pyodide-internal:pyodide-bundle/pyodide.asm.wasm";
import stdlib from "pyodide-internal:pyodide-bundle/python_stdlib.zip";

let memory = undefined;

function prepareFileSystem(Module) {
  const pymajor = 3;
  const pyminor = 11;
  Module.FS.mkdirTree("/lib");
  Module.FS.mkdirTree(`/lib/python${pymajor}.${pyminor}/site-packages`);
  Module.FS.writeFile(
    `/lib/python${pymajor}${pyminor}.zip`,
    new Uint8Array(stdlib),
    { canOwn: true }
  );
  Module.FS.mkdir(Module.API.config.env.HOME);
}

async function makeSnapshot(Module, run) {
  const imports = [
    "_pyodide.docstring",
    "_pyodide._core_docs",
    "traceback",
    "collections.abc",
    "asyncio",
    "inspect",
    "tarfile",
    "importlib.metadata",
    "re",
    "shutil",
    "sysconfig",
    "importlib.machinery",
    "pathlib",
    "site",
    "tempfile",
    "typing",
    "zipfile",
  ];
  const to_import = imports.join(",");
  const to_delete = Array.from(
    new Set(imports.map((x) => x.split(".")[0]))
  ).join(",");
  run(`import ${to_import}`);
  run("sysconfig.get_config_vars()");
  run(`del ${to_delete}`);
  memory = Module.HEAP8.slice();
}

export async function loadPyodide() {
  const API = {};
  const config = { jsglobals: globalThis, env: { HOME: "/session" } };
  API.config = config;
  const Module = {
    noInitialRun: !!memory,
    API,
    locateFile: (x) => x,
    instantiateWasm(info, receiveInstance) {
      (async function () {
        const instance = await WebAssembly.instantiate(module, info);
        receiveInstance(instance, module);
      })();
      return {};
    },
    preRun: [prepareFileSystem],
  };

  try {
    // Force Emscripten to feature detect the way we want
    globalThis.window = {};       // makes ENVIRONMENT_IS_WEB    = true
    globalThis.importScripts = 1; // makes ENVIRONMENT_IS_WORKER = false
    const p = _createPyodideModule(Module);
    delete globalThis.window;
    delete globalThis.importScripts;
    await p;
  } catch (e) {
    e.stack.split("\n").forEach(console.log.bind(console));
  }
  function run(code) {
    const [status, err] = API.rawRun(code);
    if (status) {
      console.warn("Command failed:", code);
      console.warn("Error was:");
      for (const line of err.split("\n")) {
        console.warn(line);
      }
      throw new Error("Failed");
    }
  }

  if (!memory) {
    await makeSnapshot(Module, run);
  } else {
    Module.growMemory(memory.byteLength);
    Module.HEAP8.set(new Uint8Array(memory));
  }

  let [err, captured_stderr] = API.rawRun("import _pyodide_core");
  if (err) {
    Module.API.fatal_loading_error(
      "Failed to import _pyodide_core\n",
      captured_stderr
    );
  }
  API.finalizeBootstrap();
  return API.public_api;
}
