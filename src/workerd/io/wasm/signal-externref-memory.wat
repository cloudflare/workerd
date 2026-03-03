;; Module that imports a global named "memory" typed as externref, NOT as a
;; linear memory import.  The shim's findMemory() checks WebAssembly.Module.imports()
;; and only considers descriptors whose kind is 'memory'.  An externref global
;; named "memory" will have kind 'global', so the shim must NOT register this
;; module for shutdown signal handling.

(module
  ;; Import a global named "memory" — but as externref, not as linear memory.
  (import "env" "memory" (global $mem externref))

  ;; The two globals that would normally trigger signal registration.
  (global (export "__instance_signal") i32 (i32.const 0))
  (global (export "__instance_terminated") i32 (i32.const 4))

  ;; No linear memory at all — the module cannot be registered.
  ;; Export a simple function so the test can verify instantiation succeeded.
  (func (export "get_value") (result i32)
    (i32.const 42)
  )
)
