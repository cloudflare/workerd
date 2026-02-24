;; __signal_address at 65533 leaves only 3 bytes, but we need 4.

(module
  (memory (export "memory") 1)
  (global (export "__signal_address") i32 (i32.const 65533))
  (global (export "__terminated_address") i32 (i32.const 0))
)
