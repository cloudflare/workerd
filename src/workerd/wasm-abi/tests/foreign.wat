;; Copyright (c) 2026 Cloudflare, Inc.
;; Licensed under the Apache 2.0 license found in the LICENSE file or at:
;;     https://opensource.org/licenses/Apache-2.0

;; A guest that imports from a Wasm import module that is not part of the Workers Wasm ABI.
(module
  (import "env" "f" (func))
  (memory (export "memory") 1 1)
  (func (export "handle") (param i32 i32 i32) (result i32)
    i32.const 0))
