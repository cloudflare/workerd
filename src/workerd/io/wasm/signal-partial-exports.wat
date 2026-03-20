;; Module that exports only __instance_signal but NOT __instance_terminated.
;; The shim should NOT register this module because __instance_terminated is required.

(module
  (memory (export "memory") 1)

  (global (export "__instance_signal") i32 (i32.const 0))

  (func (export "get_signal") (result i32)
    (i32.load (global.get 0))
  )
)
