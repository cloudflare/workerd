// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include "basics.h"

namespace workerd::api {

class ScheduledEvent final: public ExtendableEvent {
public:
  explicit ScheduledEvent(double scheduledTime, kj::StringPtr cron);

  static jsg::Ref<ScheduledEvent> constructor(kj::String type) = delete;

  double getScheduledTime() { return scheduledTime; }
  kj::StringPtr getCron() { return cron; }
  void noRetry();

  JSG_RESOURCE_TYPE(ScheduledEvent) {
      JSG_INHERIT(ExtendableEvent);

      JSG_READONLY_INSTANCE_PROPERTY(scheduledTime, getScheduledTime);
      JSG_READONLY_INSTANCE_PROPERTY(cron, getCron);
      JSG_METHOD(noRetry);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("cron", cron);
  }

private:
  double scheduledTime;
  kj::String cron;
};

// Type used when calling a module-exported scheduled event handler.
class ScheduledController final: public jsg::Object {
public:
  ScheduledController(jsg::Ref<ScheduledEvent> event)
      : event(kj::mv(event)) {}

  double getScheduledTime() { return event->getScheduledTime(); }
  kj::StringPtr getCron() { return event->getCron(); }
  void noRetry() { event->noRetry(); }

  JSG_RESOURCE_TYPE(ScheduledController) {
    JSG_READONLY_INSTANCE_PROPERTY(scheduledTime, getScheduledTime);
    JSG_READONLY_INSTANCE_PROPERTY(cron, getCron);
    JSG_METHOD(noRetry);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("event", event);
  }

private:
  jsg::Ref<ScheduledEvent> event;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(event);
  }
};

#define EW_SCHEDULED_ISOLATE_TYPES \
  api::ScheduledEvent,             \
  api::ScheduledController
// The list of scheduled.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
} // namespace workerd::api
