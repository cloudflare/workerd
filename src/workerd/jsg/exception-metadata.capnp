@0xa9ae63464030fcef;
# Schema for JavaScript exception metadata passed through KJ exception details

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::jsg");
$Cxx.allowCancellation;

struct JsExceptionMetadata {
  # JavaScript error type (e.g., "Error", "TypeError", "RangeError")
  errorType @0 :Text;

  # Full stack trace string as produced by V8
  # Example: "Error: User-thrown exception\n    at Object.fetch (foo:4:11)"
  stackTrace @1 :Text;
}
