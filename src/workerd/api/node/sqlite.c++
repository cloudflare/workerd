#include "sqlite.h"

#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

void SqliteUtil::backup(jsg::Lock& js) {
  JSG_FAIL_REQUIRE(Error, "backup is not implemented"_kj);
}

}  // namespace workerd::api::node
