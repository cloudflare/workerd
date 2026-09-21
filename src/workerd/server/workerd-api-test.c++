// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/server/workerd-api.h>
#include <workerd/util/capnp-util.h>

#include <kj/test.h>

namespace workerd::server {
namespace {

struct FailingErrorReporter final: public Worker::ValidationErrorReporter {
  void addError(kj::String error) override {
    KJ_FAIL_REQUIRE("unexpected error", error);
  }
  void addEntrypoint(kj::Maybe<kj::StringPtr> exportName, kj::Array<kj::String> methods) override {}
  void addActorClass(kj::StringPtr exportName) override {}
  void addWorkflowClass(kj::StringPtr exportName, kj::Array<kj::String> methods) override {}
};

KJ_TEST("readModuleConf() returns views into the config message and keeps it alive") {
  static constexpr kj::byte kData[] = {1, 2, 3};

  auto conf = buildArcMessage<config::Worker::Module>([](config::Worker::Module::Builder b) {
    b.setName("data.bin");
    b.setData(kj::arrayPtr(kData));
  });
  const kj::byte* storage = conf->getData().begin();

  // The only reference to the message is transferred to readModuleConf().
  auto module = WorkerdApi::readModuleConf(kj::mv(conf), CompatibilityFlags::Reader());

  auto& body = *module.content.get<Worker::Script::DataModule>().body;
  KJ_EXPECT(*module.name == "data.bin"_kj);
  KJ_EXPECT(body == kj::arrayPtr(kData));
  // No copy: the body points into the message.
  KJ_EXPECT(body.begin() == storage);
}

KJ_TEST("extractSource() returns views into the config message and keeps it alive") {
  auto conf = buildArcMessage<config::Worker>([](config::Worker::Builder b) {
    auto modules = b.initModules(2);
    modules[0].setName("main.js");
    modules[0].setEsModule("export default 1;");
    modules[1].setName("notes.txt");
    modules[1].setText("hello");
  });
  const char* mainStorage = conf->getModules()[0].getEsModule().begin();
  const char* textStorage = conf->getModules()[1].getText().begin();

  FailingErrorReporter errorReporter;
  auto source = WorkerdApi::extractSource(
      "worker"_kj, kj::mv(conf), CompatibilityFlags::Reader(), errorReporter);

  auto& modules = source.variant.get<Worker::Script::ModulesSource>();
  KJ_EXPECT(*modules.mainModule == "main.js"_kj);
  KJ_EXPECT(modules.modules->size() == 2);

  auto& main = (*modules.modules)[0].content.get<Worker::Script::EsModule>();
  KJ_EXPECT(main.body->begin() == mainStorage);
  KJ_EXPECT(kj::str(*main.body) == "export default 1;"_kj);

  auto& text = (*modules.modules)[1].content.get<Worker::Script::TextModule>();
  KJ_EXPECT(text.body->begin() == textStorage);
  KJ_EXPECT(*text.body == "hello"_kj);

  // A clone shares the same storage as well.
  auto clone = source.clone();
  auto& clonedMain = (*clone.variant.get<Worker::Script::ModulesSource>().modules)[0]
                         .content.get<Worker::Script::EsModule>();
  KJ_EXPECT(clonedMain.body->begin() == mainStorage);
}

}  // namespace
}  // namespace workerd::server
