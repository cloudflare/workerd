;; Copyright (c) 2026 Cloudflare, Inc.
;; Licensed under the Apache 2.0 license found in the LICENSE file or at:
;;     https://opensource.org/licenses/Apache-2.0

;; A guest that uses cf:core/v1 and cf:http/v1: it responds with the text of the binding GREETING
;; and the headers `x-guest: wasm` and `x-method: <the request's method>`. An import that fails
;; makes `handle` throw its error.
;;
;; It also imports, without calling them, the other imports that the first guests written against
;; the ABI use, so that it fails to instantiate, with a LinkError naming the import, if a module
;; loses one of them.
(module
  (import "cf:core/v1" "handle_drop" (func $handle_drop (param i32)))
  (import "cf:core/v1" "env_get"
    (func $env_get (param $env i32) (param $name_ptr i32) (param $name_len i32) (param $ret i32)
                   (result i32)))
  (import "cf:core/v1" "value_to_string"
    (func $value_to_string (param $h i32) (param $ret i32) (result i32)))
  (import "cf:core/v1" "respond" (func $respond (param $response i32) (result i32)))
  ;; new Response(body, init): body is maybe(oneof(...)) with the string at tag 1, init is
  ;; maybe(handle).
  (import "cf:http/v1" "Response.constructor"
    (func $response_new (param $present i32) (param $tag i32) (param $p0 i32) (param $p1 i32)
                        (param $init i32) (param $ret i32) (result i32)))
  (import "cf:http/v1" "Response#headers.get"
    (func $response_headers (param $self i32) (param $ret i32) (result i32)))
  (import "cf:http/v1" "Headers#set"
    (func $headers_set (param $self i32) (param $name_ptr i32) (param $name_len i32)
                       (param $value_ptr i32) (param $value_len i32) (result i32)))
  (import "cf:http/v1" "Request#method.get"
    (func $request_method (param $self i32) (param $ret i32) (result i32)))

  ;; Imported, not called.
  (import "cf:core/v1" "error_new" (func (param i32 i32 i32 i32) (result i32)))
  (import "cf:core/v1" "object_new" (func (result i32)))
  (import "cf:core/v1" "object_set" (func (param i32 i32 i32 i32) (result i32)))
  (import "cf:core/v1" "object_set_f64" (func (param i32 i32 i32 f64) (result i32)))
  (import "cf:core/v1" "object_set_string" (func (param i32 i32 i32 i32 i32) (result i32)))
  (import "cf:core/v1" "panic" (func (param i32 i32)))
  (import "cf:core/v1" "value_to_bytes" (func (param i32 i32) (result i32)))
  (import "cf:core/v1" "array_get" (func (param i32 i32 i32) (result i32)))
  (import "cf:core/v1" "console_log" (func (param i32 i32 i32)))
  (import "cf:http/v1" "Body#text" (func (param i32 i32) (result i32)))
  (import "cf:http/v1" "Headers#append" (func (param i32 i32 i32 i32 i32) (result i32)))
  (import "cf:http/v1" "Headers#forEach" (func (param i32 i32 i32) (result i32)))
  (import "cf:http/v1" "Headers#get" (func (param i32 i32 i32 i32) (result i32)))
  (import "cf:http/v1" "Headers.constructor" (func (param i32 i32) (result i32)))
  (import "cf:http/v1" "Request#headers.get" (func (param i32 i32) (result i32)))
  (import "cf:http/v1" "Request#url.get" (func (param i32 i32) (result i32)))
  (import "cf:http/v1" "Request.constructor" (func (param i32 i32 i32 i32 i32) (result i32)))
  (import "cf:http/v1" "Response#status.get" (func (param i32 i32) (result i32)))
  (import "cf:http/v1" "globalThis.fetch" (func (param i32 i32 i32 i32 i32) (result i32)))

  (memory (export "memory") 1 1)
  (data (i32.const 16) "GREETING")
  (data (i32.const 32) "x-guest")
  (data (i32.const 48) "wasm")
  (data (i32.const 56) "x-method")
  ;; The StrSlot at 80 receives the greeting into the buffer at 256 (ptr = 256, cap = 1024); the
  ;; host writes its length at 88.
  (data (i32.const 80) "\00\01\00\00\00\04\00\00")
  ;; The StrSlot at 112 receives the request's method into the buffer at 1280 (cap = 64).
  (data (i32.const 112) "\00\05\00\00\40\00\00\00")

  (func (export "handle") (param $request i32) (param $env i32) (param $ctx i32) (result i32)
    (local $status i32) (local $greeting i32) (local $response i32) (local $headers i32)
    (local.set $status (call $env_get (local.get $env) (i32.const 16) (i32.const 8) (i32.const 64)))
    (if (local.get $status) (then (return (i32.sub (i32.const 0) (local.get $status)))))
    (local.set $greeting (i32.load (i32.const 64)))
    (local.set $status (call $value_to_string (local.get $greeting) (i32.const 80)))
    (call $handle_drop (local.get $greeting))
    (if (local.get $status) (then (return (i32.sub (i32.const 0) (local.get $status)))))
    (local.set $status
      (call $response_new (i32.const 1) (i32.const 1) (i32.const 256) (i32.load (i32.const 88))
                          (i32.const 0) (i32.const 96)))
    (if (local.get $status) (then (return (i32.sub (i32.const 0) (local.get $status)))))
    (local.set $response (i32.load (i32.const 96)))
    (local.set $status (call $response_headers (local.get $response) (i32.const 100)))
    (if (local.get $status) (then (return (i32.sub (i32.const 0) (local.get $status)))))
    (local.set $headers (i32.load (i32.const 100)))
    (local.set $status
      (call $headers_set (local.get $headers) (i32.const 32) (i32.const 7) (i32.const 48)
                         (i32.const 4)))
    (if (local.get $status) (then (return (i32.sub (i32.const 0) (local.get $status)))))
    (local.set $status (call $request_method (local.get $request) (i32.const 112)))
    (if (local.get $status) (then (return (i32.sub (i32.const 0) (local.get $status)))))
    (local.set $status
      (call $headers_set (local.get $headers) (i32.const 56) (i32.const 8) (i32.const 1280)
                         (i32.load (i32.const 120))))
    (call $handle_drop (local.get $headers))
    (if (local.get $status) (then (return (i32.sub (i32.const 0) (local.get $status)))))
    (local.set $status (call $respond (local.get $response)))
    (call $handle_drop (local.get $response))
    (if (local.get $status) (then (return (i32.sub (i32.const 0) (local.get $status)))))
    (i32.const 0)))
