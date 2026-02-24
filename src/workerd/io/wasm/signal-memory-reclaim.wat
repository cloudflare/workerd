;; Large-memory module (16MB) for GC reclamation testing.
;; Instantiating many of these without reclaiming terminated ones would OOM.

(module
  (memory (export "memory") 256)

  (global (export "__signal_address") i32 (i32.const 0))
  (global (export "__terminated_address") i32 (i32.const 4))

  ;; Write non-zero to terminated address, signaling the module has exited.
  (func (export "mark_exited")
    (i32.store (global.get 1) (i32.const 1))
  )

  (func (export "get_signal") (result i32)
    (i32.load (global.get 1))
  )
)
