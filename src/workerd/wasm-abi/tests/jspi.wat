;; Copyright (c) 2026 Cloudflare, Inc.
;; Licensed under the Apache 2.0 license found in the LICENSE file or at:
;;     https://opensource.org/licenses/Apache-2.0

;; A guest whose export `run` returns what the import `wait` returns.
(module
  (import "env" "wait" (func $wait (result i32)))
  (func (export "run") (result i32)
    call $wait))
