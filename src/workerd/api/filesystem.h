// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/memory.h>
#include <workerd/jsg/url.h>

#include <kj/filesystem.h>
#include <kj/string.h>

namespace workerd::api::filesystem {

enum class NodeType : kj::uint { FILE, DIRECTORY };

class Handle: public jsg::Object {
 public:
  Handle(kj::Path path): path(kj::mv(path)) {}

  void close();

  JSG_RESOURCE_TYPE(Handle) {
    JSG_METHOD(close);
  }

 private:
  kj::Path path;
};

class Node: public jsg::Object {
 public:
  Node(kj::String name, kj::Path path, kj::Date modifiedAt, kj::Date createdAt, NodeType type)
      : name(kj::mv(name)),
        path(kj::mv(path)),
        modifiedAt(kj::mv(modifiedAt)),
        createdAt(kj::mv(createdAt)),
        type(type) {};

  virtual bool isReadable() const {
    return false;
  }
  virtual bool isWritable() const {
    return false;
  }
  virtual bool isAsyncOnly() const {
    return false;
  }
  virtual bool isSyncOnly() const {
    return false;
  }

  virtual kj::Maybe<jsg::Ref<Handle>> getFd() {
    return kj::none;
  }

  struct Stat {
    kj::StringPtr name;
    kj::StringPtr path;
    uint64_t modifiedAt;
    uint64_t createdAt;
    kj::uint type;

    JSG_STRUCT(name, path, modifiedAt, createdAt, type);
  };

  Stat getStat();

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackFieldWithSize("name", name.size());
    tracker.trackFieldWithSize("path", path.size());
  }

  JSG_RESOURCE_TYPE(Node) {
    JSG_READONLY_PROTOTYPE_PROPERTY(readable, isReadable);
    JSG_READONLY_PROTOTYPE_PROPERTY(writable, isWritable);
    JSG_READONLY_PROTOTYPE_PROPERTY(asyncOnly, isAsyncOnly);
    JSG_READONLY_PROTOTYPE_PROPERTY(syncOnly, isSyncOnly);

    JSG_METHOD(getStat);
    JSG_METHOD(getFd);
  }

 private:
  kj::String name;
  kj::Path path;
  kj::Date modifiedAt;
  kj::Date createdAt;
  NodeType type;
};

}  // namespace workerd::api::filesystem

#define EW_FILESYSTEM_ISOLATE_TYPES                                                                \
  api::filesystem::Node, api::filesystem::Node::Stat, api::filesystem::Handle
