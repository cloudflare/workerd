;; Module that exports only __instance_terminated but NOT __instance_signal.
;; The shim should register this module — __instance_signal is optional. The module
;; will receive the terminated flag when the isolate is killed, but will not receive
;; the SIGXCPU warning signal.

(module
  (memory (export "memory") 1)

  ;; terminated at byte 0.
  (global (export "__instance_terminated") i32 (i32.const 0))

  (func (export "get_terminated") (result i32)
    (i32.load (global.get 0))
  )

  ;; Write a non-zero value to the terminated field, signalling that this instance
  ;; has exited and the runtime may reclaim the linear memory.
  (func (export "mark_exited")
    (i32.store (global.get 0) (i32.const 1))
  )
)
