;; Module whose signal address is pre-initialized to a non-zero value (0xDEADBEEF)
;; via a data segment. The runtime should zero the signal field during registration,
;; so get_signal() should return 0 after instantiation.

(module
  (memory (export "memory") 1)

  ;; Signal at byte 0, terminated at byte 4.
  (global (export "__instance_signal") i32 (i32.const 0))
  (global (export "__instance_terminated") i32 (i32.const 4))

  ;; Pre-fill signal (bytes 0-3) with 0xDEADBEEF.
  (data (i32.const 0) "\EF\BE\AD\DE")

  (func (export "get_signal") (result i32)
    (i32.load (i32.const 0))
  )
)
