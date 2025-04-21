// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.
#pragma once

#include <workerd/api/filesystem.h>
#include <workerd/jsg/buffersource.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsvalue.h>
#include <workerd/jsg/memory.h>
#include <workerd/jsg/url.h>

#include <kj/string.h>

using workerd::api::filesystem::Handle;
using workerd::api::filesystem::Node;

namespace workerd::api::node {

class FilesystemUtil final: public jsg::Object {
 public:
  FilesystemUtil() = default;
  FilesystemUtil(jsg::Lock&, const jsg::Url&) {}

  class BundleNode final: public Node {
   public:
    BundleNode(kj::String name,
        kj::Path path,
        kj::Date modifiedAt,
        kj::Date createdAt,
        filesystem::NodeType type)
        : Node(kj::mv(name), kj::mv(path), modifiedAt, createdAt, type) {}

    bool isReadable() const override {
      return true;
    }
    bool isWritable() const override {
      return false;
    }
    bool isAsyncOnly() const override {
      return false;
    }
    bool isSyncOnly() const override {
      return false;
    }

    kj::Maybe<jsg::Ref<Handle>> getFd() override {
      return kj::none;
    }

    JSG_RESOURCE_TYPE(BundleNode) {
      JSG_INHERIT(Node);
    }
  };

  kj::Maybe<jsg::Ref<Node>> open(jsg::Lock&, kj::String path);

  JSG_RESOURCE_TYPE(FilesystemUtil) {
    JSG_METHOD(open);
  }
};

#define EW_NODE_FILESYSTEM_ISOLATE_TYPES                                                           \
  api::node::FilesystemUtil, api::node::FilesystemUtil::BundleNode

}  // namespace workerd::api::node
