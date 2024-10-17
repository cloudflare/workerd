@0xc00ad00cc650fb45;

struct EmscriptenSetup {
  code @0 :Text;
  pyodideAsmWasm @1 :Data;
  pythonStdlibZip @2 :Data;
}

const emscriptenSetup :EmscriptenSetup = (
  code = embed "emscriptenSetup.js",
  pyodideAsmWasm = embed "pyodide.asm.wasm",
  pythonStdlibZip = embed "python_stdlib.zip",
);
