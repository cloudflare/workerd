;; Emscripten-shaped module for wasm-snapshot-test: exported memory and indirect function table,
;; JS-driven constructors, exported mutable globals of each JS-visible kind (i32 and f64 are Numbers,
;; i64 a BigInt), an import, and a call_indirect entry point.
(module
  (type $i32_to_i32 (func (param i32) (result i32)))
  (import "env" "log" (func $log (param i32)))

  (memory (export "memory") 1 16)
  (table (export "__indirect_function_table") 4 funcref)
  (global $counter (export "counter") (mut i32) (i32.const 0))
  (global (export "big") (mut i64) (i64.const 0))
  (global (export "ratio") (mut f64) (f64.const 0))

  ;; Active data segment: re-applied on every instantiation; the snapshot must win over it.
  (data (i32.const 0) "hello")

  (func (export "__wasm_call_ctors")
    (i32.store (i32.const 16) (i32.const 42)))

  (func $inc (export "inc") (result i32)
    (i32.store (i32.const 16) (i32.add (i32.load (i32.const 16)) (i32.const 1)))
    (global.set $counter (i32.add (global.get $counter) (i32.const 1)))
    (i32.load (i32.const 16)))

  (func $get (export "get") (param i32) (result i32)
    (i32.load8_u (local.get 0)))

  (func (export "set") (param i32 i32)
    (i32.store8 (local.get 0) (local.get 1)))

  (func $twice (export "twice") (param i32) (result i32)
    (i32.mul (local.get 0) (i32.const 2)))

  (func (export "callLog") (param i32)
    (call $log (local.get 0)))

  (func (export "callIndirect") (param i32 i32) (result i32)
    (call_indirect (type $i32_to_i32) (local.get 1) (local.get 0)))

  (elem (i32.const 0) $get $twice)
)
