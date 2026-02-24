;; Module with internal memory (not exported) and signal globals.
;; Imports a function so the imports object exists, but does NOT import memory.
;; Used to verify that a WebAssembly.Memory in the imports object is not mistaken
;; for the module's linear memory.

(module
  (import "env" "log" (func $log (param i32)))

  (memory 1)

  (global (export "__signal_address") i32 (i32.const 0))
  (global (export "__terminated_address") i32 (i32.const 4))

  (func (export "get_signal") (result i32)
    (i32.load (i32.const 0))
  )
)
