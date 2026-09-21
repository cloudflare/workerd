// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/worker-source.h>
#include <workerd/util/arc-view.h>

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("WorkerSource::Module::clone() shares the EsModule body") {
  static constexpr kj::StringPtr kBody = "export default 1;"_kj;

  auto body = arcCharView(kj::str(kBody));
  WorkerSource::Module original{
    .name = arcView(kj::str("main.js")),
    .content = WorkerSource::EsModule{.body = body.addRef()},
  };

  auto clone = original.clone();

  auto& originalContent = original.content.get<WorkerSource::EsModule>();
  auto& cloneContent = clone.content.get<WorkerSource::EsModule>();

  // Same bytes, no copy: the clone's view points at the very same storage.
  KJ_ASSERT(kj::str(*cloneContent.body) == kBody);
  KJ_ASSERT(cloneContent.body->begin() == originalContent.body->begin());
}

KJ_TEST("WorkerSource bodies outlive the object they were extracted from") {
  // A module body is a view whose Arc shares the owner's refcount, so the module (and any clone
  // of it) keeps the bytes alive after every other reference to the owner is gone.
  kj::Maybe<WorkerSource::Module> maybeClone;
  const char* storage;
  {
    auto owner = kj::arc<kj::String>(kj::str("hello, world"));
    storage = owner->begin();
    WorkerSource::Module module{
      .name = arcView(kj::str("text.txt")),
      .content = WorkerSource::TextModule{.body = owner.addRef().project(
                                              [](const kj::String& s) { return s.asPtr(); })},
    };
    maybeClone = module.clone();
    // `owner` and `module` are destroyed here.
  }

  auto& body = *KJ_ASSERT_NONNULL(maybeClone).content.get<WorkerSource::TextModule>().body;
  KJ_ASSERT(body.begin() == storage);
  KJ_ASSERT(body == "hello, world"_kj);
}

}  // namespace
}  // namespace workerd
