#include "unsafe.h"

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/script.h>

namespace workerd::api {

namespace {
static constexpr auto EVAL_STR = "eval"_kjc;
static constexpr auto ANON_STR = "anonymous"_kjc;
static constexpr auto ASYNC_FN_PREFIX = "async function "_kjc;
static constexpr auto ASYNC_FN_ARG_OPEN = "("_kjc;
static constexpr auto ASYNC_FN_ARG_CLOSE = ") {"_kjc;
static constexpr auto ASYNC_FN_SUFFIX = "}"_kjc;

inline kj::StringPtr getName(jsg::Optional<kj::String>& name, kj::StringPtr def) {
  return name.map([](kj::String& str) { return str.asPtr(); }).orDefault(def);
}
}  // namespace

#ifdef WORKERD_FUZZILLI
void Stdin::reprl(jsg::Lock& js) {
  js.setAllowEval(true);
  /*
  cov_init_builtins_edges(static_cast<uint32_t>(
      v8::internal::BasicBlockProfiler::Get()
          ->GetCoverageBitmap(reinterpret_cast<v8::Isolate*>(js.v8Isolate))
          .size()));
  */

  char helo[] = "HELO";
  if (write(REPRL_CWFD, helo, 4) != 4 || read(REPRL_CRFD, helo, 4) != 4) {
    printf("Invalid HELO response from parent\n");
  }

  if (memcmp(helo, "HELO", 4) != 0) {
    printf("Invalid response from parent\n");
  }

  do {
    v8::HandleScope handle_scope(js.v8Isolate);
    v8::TryCatch try_catch(js.v8Isolate);
    try_catch.SetVerbose(true);

    size_t script_size = 0;
    unsigned action = 0;
    ssize_t nread = read(REPRL_CRFD, &action, 4);
    fflush(0);
    fflush(stderr);
    if (nread != 4 || action != 0x63657865) {  // 'exec'
      fprintf(stderr, "Unknown action: %x\n", action);
      exit(-1);
    }

    CHECK(read(REPRL_CRFD, &script_size, 8) == 8);

    char* script_ = (char*)malloc(script_size + 1);
    CHECK(script_ != nullptr);

    char* source_buffer_tail = script_;
    ssize_t remaining = (ssize_t)script_size;

    while (remaining > 0) {
      ssize_t rv = read(REPRL_DRFD, source_buffer_tail, (size_t)remaining);
      if (rv <= 0) {
        fprintf(stderr, "Failed to load script\n");
        exit(-1);
      }
      remaining -= rv;
      source_buffer_tail += rv;
    }

    script_[script_size] = '\0';

    int status = 0;
    unsigned res_val = 0;
    const kj::String script = kj::str(script_);
    const kj::String wrapped = kj::str("{", script_, "}");
    auto compiled = jsg::NonModuleScript::compile(js, wrapped, "reprl"_kj);
    try {
      auto result = compiled.runAndReturn(js);
      res_val = jsg::check(v8::Local<v8::Value>(result)->Int32Value(js.v8Context()));
      // if we reach that point execution was successful -> return 0
      res_val = 0;
    } catch (jsg::JsExceptionThrown&) {
      res_val = 11;
      if (try_catch.HasCaught()) {
        auto str = workerd::jsg::check(try_catch.Message()->Get()->ToDetailString(js.v8Context()));
        v8::String::Utf8Value utf8String(js.v8Isolate, str);
        fflush(stdout);
      }
    }

    fflush(stdout);
    fflush(stderr);
    status = (res_val & 0xFF) << 8;
    CHECK(write(REPRL_CWFD, &status, 4) == 4);
    __sanitizer_cov_reset_edgeguards();
    free(script_);
    //cleanup context

  } while (true);
}
#endif

jsg::JsValue UnsafeEval::eval(jsg::Lock& js, kj::String script, jsg::Optional<kj::String> name) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));
  auto compiled = jsg::NonModuleScript::compile(js, script, getName(name, EVAL_STR));
  return compiled.runAndReturn(js);
}

UnsafeEval::UnsafeEvalFunction UnsafeEval::newFunction(jsg::Lock& js,
    jsg::JsString script,
    jsg::Optional<kj::String> name,
    jsg::Arguments<jsg::JsRef<jsg::JsString>> args,
    const jsg::TypeHandler<UnsafeEvalFunction>& handler) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));

  auto nameStr = js.str(getName(name, ANON_STR));
  v8::ScriptOrigin origin(nameStr);
  v8::ScriptCompiler::Source source(script, origin);

  v8::LocalVector<v8::String> argNames(js.v8Isolate);
  argNames.reserve(args.size());
  for (auto& arg: args) {
    argNames.push_back(arg.getHandle(js));
  }

  auto fn = jsg::check(v8::ScriptCompiler::CompileFunction(
      js.v8Context(), &source, argNames.size(), argNames.data(), 0, nullptr));
  fn->SetName(nameStr);

  return KJ_ASSERT_NONNULL(handler.tryUnwrap(js, fn));
}

UnsafeEval::UnsafeEvalFunction UnsafeEval::newAsyncFunction(jsg::Lock& js,
    jsg::JsString script,
    jsg::Optional<kj::String> name,
    jsg::Arguments<jsg::JsRef<jsg::JsString>> args,
    const jsg::TypeHandler<UnsafeEvalFunction>& handler) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));

  auto nameStr = js.str(getName(name, ANON_STR));

  // This case is sadly a bit more complicated than the newFunction variant
  // because v8 currently (surprisingly) does not actually expose a way
  // CompileAsyncFunction variant (silly v8). What we end up doing here is
  // building a string that wraps the script provided by the caller:
  //
  //   async function {name}({args}) { {script} }; {name}
  //
  // Where {name} is the name of the function as provided by the user or
  // "anonymous" by default, {args} is the list of args provided by the
  // caller, if any. We end the constructed string with the name of the
  // function so that the result of running the compiled script is a reference
  // to the compiled function.

  auto prepared = v8::String::Concat(js.v8Isolate, js.strIntern(ASYNC_FN_PREFIX), nameStr);
  prepared = v8::String::Concat(js.v8Isolate, prepared, js.strIntern(ASYNC_FN_ARG_OPEN));

  for (auto& arg: args) {
    prepared = v8::String::Concat(js.v8Isolate, prepared, arg.getHandle(js));
    prepared = v8::String::Concat(js.v8Isolate, prepared, js.strIntern(","));
  }
  prepared = v8::String::Concat(js.v8Isolate, prepared, js.strIntern(ASYNC_FN_ARG_CLOSE));
  prepared = v8::String::Concat(js.v8Isolate, prepared, script);
  prepared = v8::String::Concat(js.v8Isolate, prepared, js.strIntern(ASYNC_FN_SUFFIX));
  prepared = v8::String::Concat(js.v8Isolate, prepared, js.strIntern(";"));
  prepared = v8::String::Concat(js.v8Isolate, prepared, nameStr);

  v8::ScriptOrigin origin(nameStr);
  v8::ScriptCompiler::Source source(prepared, origin);

  auto compiled = jsg::check(v8::ScriptCompiler::Compile(js.v8Context(), &source));
  auto result = jsg::check(compiled->Run(js.v8Context()));

  KJ_REQUIRE(result->IsAsyncFunction());

  return KJ_ASSERT_NONNULL(handler.tryUnwrap(js, result.As<v8::Function>()));
}

jsg::JsValue UnsafeEval::newWasmModule(jsg::Lock& js, kj::Array<kj::byte> src) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));

  auto maybeWasmModule = v8::WasmModuleObject::Compile(
      js.v8Isolate, v8::MemorySpan<const uint8_t>(src.begin(), src.size()));
  return jsg::JsValue(jsg::check(maybeWasmModule));
}

jsg::Promise<void> UnsafeModule::abortAllDurableObjects(jsg::Lock& js) {
  auto& context = IoContext::current();

  auto exception = JSG_KJ_EXCEPTION(FAILED, Error, "Application called abortAllDurableObjects().");
  context.abortAllActors(exception);

  // We used to perform the abort asynchronously, but that became no longer necessary when
  // `Worker::Actor`'s destructor stopped requiring taking the isolate lock.
  return js.resolvedPromise();
}

}  // namespace workerd::api
