#include "features.h"
#include "worker.h"

namespace workerd {

CompatibilityFlags::Reader FeatureFlags::get(jsg::Lock&) {
  // Note that the jsg::Lock& argument here is not actually used. We require
  // that a jsg::Lock reference is passed in as proof that current() is called
  // from within a valid isolate lock so that the Worker::ApiIsolate::current()
  // call below will work as expected.
  // TODO(later): Use of Worker::ApiIsolate::current() here implies that there
  // is only one set of compatibility flags relevant at a time within each thread
  // context. For now that holds true. Later it is possible that may not be the
  // case which will require us to further adapt this model.
  return Worker::ApiIsolate::current().getFeatureFlags();
}

}  // namespace workerd
