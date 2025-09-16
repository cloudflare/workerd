// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/common.h>

namespace workerd::api {

class Immediate;

namespace node {
class SqliteUtil final: public jsg::Object {
 public:
  SqliteUtil() = default;
  SqliteUtil(jsg::Lock&, const jsg::Url&) {}

  class DatabaseSync final: public jsg::Object {
   public:
    DatabaseSync() = default;
    DatabaseSync(jsg::Lock&, const jsg::Url&) {}

    // We intentionally do not implement the full API surface yet.
    // TODO(soon): Add the full implementation of DatabaseSync.
    static jsg::Ref<DatabaseSync> constructor(jsg::Lock& js) = delete;

    JSG_RESOURCE_TYPE(DatabaseSync) {}
  };

  class StatementSync final: public jsg::Object {
   public:
    StatementSync() = default;
    StatementSync(jsg::Lock&, const jsg::Url&) {}

    // We intentionally do not implement the full API surface yet.
    // TODO(soon): Add the full implementation of StatementSync.
    static jsg::Ref<StatementSync> constructor(jsg::Lock& js) = delete;

    JSG_RESOURCE_TYPE(StatementSync) {}
  };

  void backup(jsg::Lock& js);

  static constexpr uint8_t SQLITE_CHANGESET_OMIT = 0;
  static constexpr uint8_t SQLITE_CHANGESET_REPLACE = 1;
  static constexpr uint8_t SQLITE_CHANGESET_ABORT = 2;
  static constexpr uint8_t SQLITE_CHANGESET_DATA = 1;
  static constexpr uint8_t SQLITE_CHANGESET_NOTFOUND = 2;
  static constexpr uint8_t SQLITE_CHANGESET_CONFLICT = 3;
  static constexpr uint8_t SQLITE_CHANGESET_CONSTRAINT = 4;
  static constexpr uint8_t SQLITE_CHANGESET_FOREIGN_KEY = 5;

  JSG_RESOURCE_TYPE(SqliteUtil) {
    JSG_NESTED_TYPE(DatabaseSync);
    JSG_NESTED_TYPE(StatementSync);
    JSG_METHOD(backup);

    JSG_STATIC_CONSTANT(SQLITE_CHANGESET_OMIT);
    JSG_STATIC_CONSTANT(SQLITE_CHANGESET_REPLACE);
    JSG_STATIC_CONSTANT(SQLITE_CHANGESET_ABORT);
    JSG_STATIC_CONSTANT(SQLITE_CHANGESET_DATA);
    JSG_STATIC_CONSTANT(SQLITE_CHANGESET_NOTFOUND);
    JSG_STATIC_CONSTANT(SQLITE_CHANGESET_CONFLICT);
    JSG_STATIC_CONSTANT(SQLITE_CHANGESET_CONSTRAINT);
    JSG_STATIC_CONSTANT(SQLITE_CHANGESET_FOREIGN_KEY);
  }
};

#define EW_NODE_SQLITE_ISOLATE_TYPES                                                               \
  api::node::SqliteUtil, api::node::SqliteUtil::DatabaseSync, api::node::SqliteUtil::StatementSync
}  // namespace node

}  // namespace workerd::api
