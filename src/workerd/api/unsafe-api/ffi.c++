// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/util/autogate.h>

#include <v8.h>

#include <kj/string.h>

#ifdef WORKERD_FUZZILLI
// Defined in src/workerd/api/fuzzilli.c++.
void __sanitizer_cov_reset_edgeguards();
#endif

namespace workerd::api::unsafe_api {

bool is_test_autogate_enabled() {
  return util::Autogate::isEnabled(util::AutogateKey::TEST_WORKERD);
}

int32_t reprl_execute(v8::Isolate* isolate, ::rust::Slice<const uint8_t> script) {
  v8::HandleScope handleScope(isolate);
  // NOLINTNEXTLINE(custom-raw-v8-trycatch): Fuzzilli loop: only reports what was thrown.
  v8::TryCatch tryCatch(isolate);
  tryCatch.SetVerbose(true);
  auto context = isolate->GetCurrentContext();

  auto wrapped =
      kj::str("{", kj::arrayPtr(reinterpret_cast<const char*>(script.data()), script.size()), "}");
  v8::Local<v8::String> source;
  if (!v8::String::NewFromUtf8(isolate, wrapped.begin(), v8::NewStringType::kNormal, wrapped.size())
           .ToLocal(&source)) {
    return REPRL_COMPILE_FAILED;
  }
  v8::ScriptOrigin origin(v8::String::NewFromUtf8Literal(isolate, "reprl"));
  v8::ScriptCompiler::Source compileSource(source, origin);
  v8::Local<v8::UnboundScript> compiled;
  if (!v8::ScriptCompiler::CompileUnboundScript(isolate, &compileSource).ToLocal(&compiled)) {
    return REPRL_COMPILE_FAILED;
  }

  v8::Local<v8::Value> result;
  if (!compiled->BindToCurrentContext()->Run(context).ToLocal(&result) ||
      result->Int32Value(context).IsNothing()) {
    return 11;
  }
  return 0;
}

void reprl_reset_edge_guards() {
#ifdef WORKERD_FUZZILLI
  __sanitizer_cov_reset_edgeguards();
#endif
}

}  // namespace workerd::api::unsafe_api
