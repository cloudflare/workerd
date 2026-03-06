;; Large-memory module (16MB) for GC reclamation testing of signal-only modules.
;; Exports only __instance_signal (no __instance_terminated). Cleanup relies on
;; the weak instanceRef — when the instance is GC'd, the strong memory reference
;; is released. Instantiating many of these without GC reclamation would OOM.

(module
  (memory (export "memory") 256)

  (global (export "__instance_signal") i32 (i32.const 0))

  (func (export "get_signal") (result i32)
    (i32.load (global.get 0))
  )
)
