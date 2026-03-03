;; __instance_signal points beyond memory bounds (70000 > 65536).

(module
  (memory (export "memory") 1)
  (global (export "__instance_signal") i32 (i32.const 70000))
  (global (export "__instance_terminated") i32 (i32.const 70004))
)
