;; Module that exports memory but neither __instance_signal nor __instance_terminated.
;; The shim should NOT register this module — it should instantiate without error.

(module
  (memory (export "memory") 1)

  (func (export "add") (param i32 i32) (result i32)
    (i32.add (local.get 0) (local.get 1))
  )
)
