;; __instance_signal at 65533 leaves only 3 bytes, but we need 4.

(module
  (memory (export "memory") 1)
  (global (export "__instance_signal") i32 (i32.const 65533))
  (global (export "__instance_terminated") i32 (i32.const 0))
)
