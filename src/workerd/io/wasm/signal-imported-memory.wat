;; Module that imports memory rather than defining its own.
;; The shim must find the memory in the imports object via Module.imports().

(module
  (import "env" "memory" (memory 1))

  (global (export "__signal_address") i32 (i32.const 0))
  (global (export "__terminated_address") i32 (i32.const 4))

  (func (export "get_signal") (result i32)
    (i32.load (global.get 0))
  )
)
