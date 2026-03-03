;; Both addresses fit exactly at the boundary of a 1-page (65536-byte) memory.
;;   __instance_signal = 65528 (4 bytes from 65528..65531)
;;   __instance_terminated = 65532 (4 bytes from 65532..65535)

(module
  (memory (export "memory") 1)
  (global (export "__instance_signal") i32 (i32.const 65528))
  (global (export "__instance_terminated") i32 (i32.const 65532))

  (func (export "get_signal") (result i32)
    (i32.load (global.get 0))
  )
)
