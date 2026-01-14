declare module 'pyodide-internal:introspection.py' {
  const value: Uint8Array
  export default value
}

declare module 'pyodideRuntime-internal:emscriptenSetup' {
  function instantiateEmscriptenModule(
    isWorkerd: boolean,
    a: any,
    b: any,
    UnsafeEval: any,
  ): Promise<Module>
  export { instantiateEmscriptenModule }
}

declare module 'pyodideRuntime-internal:python_stdlib.zip' {
  const value: Uint8Array
  export default value
}

declare module 'pyodideRuntime-internal:pyodide.asm.wasm' {
  const value: Uint8Array
  export default value
}
