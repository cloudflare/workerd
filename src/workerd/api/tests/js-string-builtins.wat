;; Only links if the runtime compiles Wasm with
;; { builtins: ['js-string'], importedStringConstants: 'wasm:js/string-constants' }.
(module
  (import "wasm:js-string" "length"
    (func $length (param externref) (result i32)))

  (import "wasm:js/string-constants" "hello world"
    (global $hello externref))

  (func (export "constantLength") (result i32)
    (call $length (global.get $hello)))
)
