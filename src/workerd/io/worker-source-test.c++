// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/worker-source.h>

#include <rust/cxx.h>

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("WorkerSource::Module::clone() deep-copies an owned EsModule body") {
  static constexpr kj::StringPtr kBody = "export default 1;"_kj;

  // An EsModule that owns its body, as produced by the TypeScript transpile
  // path (see readModuleConf in workerd-api.c++): `body` is a view over
  // exactly the owned buffer.
  ::rust::String ownBody(kBody.begin(), kBody.size());
  kj::ArrayPtr<const char> bodyView(ownBody.data(), ownBody.size());
  WorkerSource::Module original{
    .name = "main.js"_kj,
    .content = WorkerSource::EsModule{.body = bodyView, .ownBody = kj::mv(ownBody)},
  };

  auto clone = original.clone();

  auto& originalContent = original.content.get<WorkerSource::EsModule>();
  auto& cloneContent = clone.content.get<WorkerSource::EsModule>();

  // The clone carries the same text...
  KJ_ASSERT(kj::str(cloneContent.body) == kBody);

  // ...and its view points into its own copy of the buffer, not into the
  // original's buffer: the clone may outlive the original.
  auto& cloneOwn = KJ_ASSERT_NONNULL(cloneContent.ownBody);
  KJ_ASSERT(cloneContent.body.begin() == cloneOwn.data());
  KJ_ASSERT(cloneContent.body.size() == cloneOwn.size());
  KJ_ASSERT(cloneContent.body.begin() != originalContent.body.begin());
}

KJ_TEST("WorkerSource::Module::clone() keeps borrowed EsModule bodies as external pointers") {
  static constexpr kj::StringPtr kBody = "export default 2;"_kj;

  // An EsModule that borrows its body from external memory (the common case:
  // the body points into a long-lived capnp config buffer). Per the
  // WorkerSource::clone() contract, external pointers are kept as-is.
  WorkerSource::Module borrowed{
    .name = "borrowed.js"_kj,
    .content = WorkerSource::EsModule{.body = kBody.asArray()},
  };

  auto clone = borrowed.clone();

  auto& cloneContent = clone.content.get<WorkerSource::EsModule>();
  KJ_ASSERT(cloneContent.body.begin() == kBody.begin());
  KJ_ASSERT(cloneContent.body.size() == kBody.size());
  KJ_ASSERT(cloneContent.ownBody == kj::none);
}

}  // namespace
}  // namespace workerd
