(module
  (type (func (param i32) (result i32)))
  (import "a" (item "f" (func (type 0))) (item "g" (global i32)))
  (import "b" (item "x") (item "y") (func (type 0)))
  (func (export "run") (param i32) (result i32)
    (i32.add (i32.add (i32.add (call 0 (local.get 0)) (call 1 (local.get 0)))
      (call 2 (local.get 0))) (global.get 0))))
