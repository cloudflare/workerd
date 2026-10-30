// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/server/docker-api.capnp.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::server {
namespace {

KJ_TEST("ContainerCreateRequest leaves memory swappiness unset by default") {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto root = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  root.setImage("test-image");
  root.initHostConfig().setPublishAllPorts(true);

  auto json = codec.encode(root);
  KJ_EXPECT(!json.asPtr().contains("\"MemorySwappiness\""));
  KJ_EXPECT(json.asPtr().contains("\"PublishAllPorts\":true"));
  KJ_EXPECT(json.asPtr().contains("\"StopTimeout\":10"));
}

KJ_TEST("ContainerCreateRequest preserves explicit memory swappiness including zero") {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto root = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  auto swappiness = root.initHostConfig().initMemorySwappiness();

  swappiness.setNumber(0);
  KJ_EXPECT(codec.encode(root).asPtr().contains("\"MemorySwappiness\":0"));
  swappiness.setNumber(60);
  KJ_EXPECT(codec.encode(root).asPtr().contains("\"MemorySwappiness\":60"));
}

}  // namespace
}  // namespace workerd::server
