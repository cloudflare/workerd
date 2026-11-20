// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd {
namespace {

// An empty module exporting nothing: the magic number and version 1.
constexpr kj::byte EMPTY_MODULE[] = {0, 'a', 's', 'm', 1, 0, 0, 0};

// What the embedder saw, and how it answers.
struct Embedder {
  kj::Vector<kj::Array<const kj::byte>> submitted;
  kj::Maybe<kj::StringPtr> refusal;
  kj::Maybe<kj::StringPtr> rejection;
  // Never settle a submission.
  bool hang = false;
  kj::Maybe<kj::String> abortedIsolate;
};

struct RecordingIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
  RecordingIoChannelFactory(TimerChannel& timer, Embedder& embedder)
      : DummyIoChannelFactory(timer),
        embedder(embedder) {}

  kj::Promise<void> submitWasmModule(kj::Array<const kj::byte> bytes) override {
    KJ_IF_SOME(message, embedder.refusal) {
      JSG_FAIL_REQUIRE(Error, message);
    }
    embedder.submitted.add(kj::mv(bytes));
    KJ_IF_SOME(message, embedder.rejection) {
      return JSG_KJ_EXCEPTION(FAILED, Error, message);
    }
    if (embedder.hang) return kj::NEVER_DONE;
    // Resolve on a later turn, so a compilation that doesn't wait for the embedder would win.
    return kj::evalLater([]() {});
  }

  void abortIsolate(kj::StringPtr reason) override {
    embedder.abortedIsolate = kj::str(reason);
  }

  Embedder& embedder;
};

struct Flags {
  bool dynamic = true;
  bool streaming = true;
};

// Runs `source`'s fetch handler once and returns the response body.
kj::String runRequest(Embedder& embedder, Flags flags, kj::StringPtr source) {
  capnp::MallocMessageBuilder flagsMessage;
  auto featureFlags = flagsMessage.initRoot<CompatibilityFlags>();
  featureFlags.setWebassemblyDynamicCompilation(flags.dynamic);
  featureFlags.setWebassemblyStreaming(flags.streaming);
  featureFlags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({
    .featureFlags = featureFlags.asReader(),
    .mainModuleSource = source,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RecordingIoChannelFactory>(timer, embedder);
  }),
  });
  auto response = fixture.runRequest(kj::HttpMethod::GET, "http://www.example.com/"_kj, ""_kj);
  KJ_EXPECT(response.statusCode == 200, response.body);
  return kj::mv(response.body);
}

void expectEmptyModule(kj::ArrayPtr<const kj::byte> bytes) {
  KJ_EXPECT(bytes == kj::arrayPtr(EMPTY_MODULE));
}

KJ_TEST("compile() submits a copy of the bytes and compiles the copy") {
  Embedder embedder;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      async fetch() {
        const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
        const promise = WebAssembly.compile(bytes);
        // The module compiles from the bytes the embedder saw.
        bytes[0] = 1;
        const module = await promise;
        return new Response(String(module instanceof WebAssembly.Module));
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "true");
  KJ_ASSERT(embedder.submitted.size() == 1);
  expectEmptyModule(embedder.submitted[0]);
}

KJ_TEST("instantiate() with bytes submits them") {
  Embedder embedder;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      async fetch() {
        const { module, instance } =
            await WebAssembly.instantiate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
        // Instantiating a compiled Module compiles nothing.
        await WebAssembly.instantiate(module);
        return new Response(String(instance instanceof WebAssembly.Instance));
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "true");
  KJ_ASSERT(embedder.submitted.size() == 1);
  expectEmptyModule(embedder.submitted[0]);
}

KJ_TEST("A rejected submission fails the compilation") {
  Embedder embedder;
  embedder.rejection = "module rejected"_kj;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      async fetch() {
        try {
          await WebAssembly.compile(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
          return new Response("compiled");
        } catch (e) {
          return new Response(e.message);
        }
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "module rejected");
}

KJ_TEST("A module the embedder refuses doesn't compile") {
  Embedder embedder;
  embedder.refusal = "module refused"_kj;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      async fetch() {
        const results = [];
        try {
          await WebAssembly.compile(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
          results.push("compiled");
        } catch (e) {
          results.push(e.message);
        }
        try {
          new WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
          results.push("compiled");
        } catch (e) {
          results.push(e.message);
        }
        return new Response(results.join("\n"));
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "module refused\nmodule refused");
}

KJ_TEST("new WebAssembly.Module() submits the bytes without waiting") {
  Embedder embedder;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      fetch() {
        const module = new WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
        return new Response(String(module instanceof WebAssembly.Module));
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "true");
  KJ_ASSERT(embedder.submitted.size() == 1);
  expectEmptyModule(embedder.submitted[0]);
}

KJ_TEST(
    "A module new WebAssembly.Module() compiled and the embedder rejects condemns the isolate") {
  Embedder embedder;
  embedder.rejection = "module rejected"_kj;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      async fetch(request, env, ctx) {
        const module = new WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
        // Keeps the request going until the rejection aborts it.
        ctx.waitUntil(new Promise(() => {}));
        return new Response(String(module instanceof WebAssembly.Module));
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "true");
  KJ_EXPECT(embedder.abortedIsolate != kj::none);
}

KJ_TEST("WebAssembly.Module() without new throws without submitting the bytes") {
  Embedder embedder;
  embedder.rejection = "module rejected"_kj;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      fetch() {
        try {
          WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
          return new Response("constructed");
        } catch (e) {
          return new Response(e.name);
        }
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "TypeError");
  KJ_EXPECT(embedder.submitted.size() == 0);
  KJ_EXPECT(embedder.abortedIsolate == kj::none);
}

KJ_TEST("A subclass of WebAssembly.Module submits the bytes") {
  Embedder embedder;
  auto body = runRequest(embedder, {}, R"JS(
    class MyModule extends WebAssembly.Module {}
    export default {
      fetch() {
        const module = new MyModule(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
        return new Response(String(module instanceof MyModule &&
            module instanceof WebAssembly.Module &&
            WebAssembly.Module.exports(module).length === 0));
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "true");
  KJ_ASSERT(embedder.submitted.size() == 1);
  expectEmptyModule(embedder.submitted[0]);
}

KJ_TEST("compileStreaming() submits the whole body before settling") {
  Embedder embedder;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      async fetch() {
        const body = new ReadableStream({
          start(controller) {
            controller.enqueue(new Uint8Array([0, 97, 115, 109]));
            controller.enqueue(new Uint8Array([1, 0, 0, 0]));
            controller.close();
          },
        });
        const response = new Response(body, { headers: { 'Content-Type': 'application/wasm' } });
        const module = await WebAssembly.compileStreaming(response);
        return new Response(String(module instanceof WebAssembly.Module));
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "true");
  KJ_ASSERT(embedder.submitted.size() == 1);
  expectEmptyModule(embedder.submitted[0]);
}

KJ_TEST(
    "Without the flags, compiling while handling a request is disallowed and streaming is absent") {
  Embedder embedder;
  auto body = runRequest(embedder, {.dynamic = false, .streaming = false}, R"JS(
    export default {
      async fetch() {
        const results = [typeof WebAssembly.compileStreaming, typeof WebAssembly.instantiateStreaming];
        try {
          await WebAssembly.compile(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
          results.push("compiled");
        } catch (e) {
          results.push(e.name);
        }
        return new Response(results.join(" "));
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "undefined undefined CompileError");
  KJ_EXPECT(embedder.submitted.size() == 0);
}

KJ_TEST("A submission pending when its request ends is dropped") {
  Embedder embedder;
  embedder.hang = true;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      fetch() {
        WebAssembly.compile(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
        return new Response("started");
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "started");
  KJ_EXPECT(embedder.submitted.size() == 1);
}

KJ_TEST("Concurrent compiles in one request all settle") {
  Embedder embedder;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      async fetch() {
        const modules = await Promise.all(Array.from({ length: 50 }, () =>
            WebAssembly.compile(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]))));
        return new Response(String(modules.every((m) => m instanceof WebAssembly.Module)));
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "true");
  KJ_EXPECT(embedder.submitted.size() == 50);
}

KJ_TEST("compile() rejects a DataView without submitting it") {
  Embedder embedder;
  auto body = runRequest(embedder, {}, R"JS(
    export default {
      async fetch() {
        const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
        try {
          await WebAssembly.compile(new DataView(bytes.buffer));
          return new Response("compiled");
        } catch (e) {
          return new Response(e.name);
        }
      },
    };
  )JS"_kj);
  KJ_EXPECT(body == "TypeError");
  KJ_EXPECT(embedder.submitted.size() == 0);
}

}  // namespace
}  // namespace workerd
