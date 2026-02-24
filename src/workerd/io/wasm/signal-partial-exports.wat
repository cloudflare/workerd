;; Module that exports only __signal_address but NOT __terminated_address.
;; The shim should NOT register this module because both globals are required.

(module
  (memory (export "memory") 1)

  (global (export "__signal_address") i32 (i32.const 0))

  (func (export "get_signal") (result i32)
    (i32.load (global.get 0))
  )
)
