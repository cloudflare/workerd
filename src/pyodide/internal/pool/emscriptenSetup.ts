/**
 * This file is intended to be executed in the Python pool (once it exists). As such, it cannot
 * import anything that transitively uses C++ extension modules. It has to work in a vanilla v8
 * isolate. Also, we will have to bundle this file and all of its transitive imports into a single
 * js file.
 */

/**
 * _createPyodideModule and pyodideWasmModule together are produced by the
 * Emscripten linker
 */
import { _createPyodideModule } from 'pyodide-internal:generated/pyodide.asm'

import {
  finishSetup,
  setGetRandomValues,
  setSetTimeout,
  setUnsafeEval,
} from 'pyodide-internal:pool/builtin_wrappers'

import { getSentinelImport } from 'pyodide-internal:pool/sentinel'

/**
 * A preRun hook. Make sure environment variables are visible at runtime.
 */
function setEnv(Module: Module): void {
  Object.assign(Module.ENV, Module.API.config.env)
}

function getWaitForDynlibs(resolveReadyPromise: PreRunHook): PreRunHook {
  return function waitForDynlibs(Module: Module): void {
    // Block the instantiation of the runtime until we can preload the dynamic libraries. The
    // promise returned by _createPyodideModule won't resolve until we call
    // `removeRunDependency('dynlibs')` so we use `emscriptenSettings.readyPromise` to continue
    // execution when we've gotten to this point.
    Module.addRunDependency('dynlibs')
    resolveReadyPromise(Module)
  }
}

function computeVersionTuple(Module: Module): [number, number, number] {
  if (Module._py_version_major) {
    const pymajor = Module._py_version_major()
    const pyminor = Module._py_version_minor()
    const micro = Module._py_version_micro()
    return [pymajor, pyminor, micro]
  }
  const versionInt = Module.HEAPU32[Module._Py_Version >>> 2]
  const major = (versionInt >>> 24) & 0xff
  const minor = (versionInt >>> 16) & 0xff
  const micro = (versionInt >>> 8) & 0xff
  return [major, minor, micro]
}

/**
 * This is passed as a preRun hook in EmscriptenSettings, run just before
 * main(). It ensures that the file system includes the stuff that main() needs,
 * most importantly the Python standard library.
 *
 * Put the Python + Pyodide standard libraries into a zip file in the
 * appropriate location /lib/python311.zip . Python will import stuff directly
 * from this zip file using ZipImporter.
 *
 * ZipImporter is quite useful here -- the Python runtime knows how to unpack a
 * bunch of different archive formats but it is not possible to use these until
 * the runtime state is initialized. So ZipImporter breaks this bootstrapping
 * knot for us.
 *
 * We also make an empty home directory and an empty global site-packages
 * directory `/lib/pythonv.vv/site-packages`.
 *
 * This is a simplified version of the `prepareFileSystem` function here:
 * https://github.com/pyodide/pyodide/blob/main/src/js/module.ts
 */
function getPrepareFileSystem(pythonStdlib: ArrayBuffer): PreRunHook {
  return function prepareFileSystem(Module: Module): void {
    Module.API.pyVersionTuple = computeVersionTuple(Module)
    const [pymajor, pyminor] = Module.API.pyVersionTuple
    Module.FS.sitePackages = `/lib/python${pymajor}.${pyminor}/site-packages`
    Module.LD_LIBRARY_PATH = [
      '/usr/lib',
      Module.FS.sitePackages,
      '/session/metadata/python_modules/lib/',
    ].join(':')
    Module.ENV.LD_LIBRARY_PATH = Module.LD_LIBRARY_PATH
    Module.FS.sessionSitePackages = `/session${Module.FS.sitePackages}`
    Module.FS.mkdirTree(Module.FS.sitePackages)
    Module.FS.writeFile(
      `/lib/python${pymajor}${pyminor}.zip`,
      new Uint8Array(pythonStdlib),
      { canOwn: true },
    )
    Module.FS.mkdirTree(Module.API.config.env.HOME)
  }
}

/**
 * A hook that the Emscripten runtime calls to perform the WebAssembly
 * instantiation action. Once instantiated, this callback function should call
 * ``successCallback()`` with the generated WebAssembly Instance object.
 *
 * @param wasmImports a JS object which contains all the function imports that
 * need to be passed to the WebAssembly Module when instantiating
 * @param successCallback A callback to indicate that instantiation was
 * successful,
 * @returns The return value of this function should contain the ``exports`` object of
 * the instantiated WebAssembly Module, or an empty dictionary object ``{}`` if
 * the instantiation is performed asynchronously, or ``false`` if instantiation
 * synchronously failed. There is no way to indicate asynchronous failure.
 */
function getInstantiateWasm(
  pyodideWasmModule: WebAssembly.Module,
): EmscriptenSettings['instantiateWasm'] {
  const sentinelImportPromise = getSentinelImport()
  return function instantiateWasm(
    wasmImports: WebAssembly.Imports,
    successCallback: (
      inst: WebAssembly.Instance,
      mod: WebAssembly.Module,
    ) => void,
  ): WebAssembly.Exports {
    ;(async (): Promise<void> => {
      wasmImports.sentinel = await sentinelImportPromise
      // Instantiate pyodideWasmModule with wasmImports
      const instance = await WebAssembly.instantiate(
        pyodideWasmModule,
        wasmImports,
      )
      successCallback(instance, pyodideWasmModule)
    })().catch((e: unknown) => {
      console.error(
        'Internal error: wasm instantiation failed. This should never happen.',
        e,
      )
      // Execution hangs at this point.
    })

    return {}
  }
}

/**
 * The Emscripten settings object
 *
 * This isn't public API of Pyodide so it's a bit fiddly.
 */
function getEmscriptenSettings(
  isWorkerd: boolean,
  pythonStdlib: ArrayBuffer,
  pyodideWasmModule: WebAssembly.Module,
): EmscriptenSettings {
  const config: PyodideConfig = {
    // jsglobals is used for the js module.
    jsglobals: globalThis,
    // environment variables go here
    env: {
      HOME: '/session',
      // We don't have access to entropy at startup so we cannot support hash
      // randomization. Setting `PYTHONHASHSEED` disables it. See further
      // discussion in topLevelEntropy/entropy_patches.py
      PYTHONHASHSEED: '111',
    },
    lockFileURL: '',
  }
  let lockFilePromise
  if (isWorkerd) {
    lockFilePromise = new Promise(
      (res) => (config.resolveLockFilePromise = res),
    )
  }
  const API = { config, lockFilePromise }
  let resolveReadyPromise: (mod: Module) => void
  let rejectReadyPromise: (e: any) => void = () => {}
  const readyPromise: Promise<Module> = new Promise((res, rej) => {
    resolveReadyPromise = res
    rejectReadyPromise = rej
  })
  const waitForDynlibs = getWaitForDynlibs(resolveReadyPromise!)
  const prepareFileSystem = getPrepareFileSystem(pythonStdlib)
  const instantiateWasm = getInstantiateWasm(pyodideWasmModule)

  // Emscripten settings to control runtime instantiation.
  return {
    // preRun hook to set up the file system before running main
    // The preRun hook gets run independently of noInitialRun, which is
    // important because the file system lives outside of linear memory.
    preRun: [prepareFileSystem, setEnv, waitForDynlibs],
    instantiateWasm,
    reportUndefinedSymbolsNoOp(): void {},
    readyPromise,
    rejectReadyPromise,
    API, // Pyodide requires we pass this in.
  }
}

/**
 * Force Emscripten to feature detect the way we want.
 * We want it to think we're the browser main thread.
 */
/* eslint-disable @typescript-eslint/no-unsafe-member-access, @typescript-eslint/no-unsafe-assignment */
function* featureDetectionMonkeyPatchesContextManager(): Generator<void> {
  const global = globalThis as any
  // Make Emscripten think we're in the browser main thread
  global.window = { sessionStorage: {} }
  global.document = { createElement(): void {} }
  global.sessionStorage = {}
  // Make Emscripten think we're not in a worker
  global.importScripts = 1
  global.WorkerGlobalScope = undefined
  try {
    yield
  } finally {
    delete global.window
    delete global.document
    delete global.sessionStorage
    delete global.importScripts
  }
}
/* eslint-enable @typescript-eslint/no-unsafe-member-access, @typescript-eslint/no-unsafe-assignment */

/**
 * Simple wrapper around _createPyodideModule that applies some monkey patches
 * to force the environment to be detected the way we want.
 *
 * In the long run we should fix this in `pyodide.asm.js` instead.
 *
 * Returns the instantiated emscriptenModule object.
 */
export async function instantiateEmscriptenModule(
  isWorkerd: boolean,
  pythonStdlib: ArrayBuffer,
  wasmModule: WebAssembly.Module,
): Promise<Module> {
  const emscriptenSettings = getEmscriptenSettings(
    isWorkerd,
    pythonStdlib,
    wasmModule,
  )
  for (const _ of featureDetectionMonkeyPatchesContextManager()) {
    // Ignore the returned promise, it won't resolve until we're done preloading dynamic
    // libraries.
    const _promise = _createPyodideModule(emscriptenSettings).catch((e) =>
      emscriptenSettings.rejectReadyPromise(e),
    )
  }

  // Wait until we've executed all the preRun hooks before proceeding
  const emscriptenModule = await emscriptenSettings.readyPromise
  emscriptenModule.setUnsafeEval = setUnsafeEval
  emscriptenModule.setGetRandomValues = setGetRandomValues
  emscriptenModule.setSetTimeout = setSetTimeout
  finishSetup()
  return emscriptenModule
}
