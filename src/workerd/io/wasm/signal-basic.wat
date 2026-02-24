;; Basic WASM module exporting __signal_address, __terminated_address, and memory.
;; Used to verify that the registration shim detects both globals and registers
;; the module, and that the sync WebAssembly.Instance constructor also registers.

(module
  (memory (export "memory") 1)

  (global (export "__signal_address") i32 (i32.const 0))
  (global (export "__terminated_address") i32 (i32.const 4))

  (func (export "get_signal") (result i32)
    (i32.load (global.get 0))
  )
)
