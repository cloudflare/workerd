;; __signal_address points beyond memory bounds (70000 > 65536).

(module
  (memory (export "memory") 1)
  (global (export "__signal_address") i32 (i32.const 70000))
  (global (export "__terminated_address") i32 (i32.const 70004))
)
