#pragma once

#include <kj/string.h>

namespace workerd {

enum class UncaughtExceptionSource {
  INTERNAL,
  INTERNAL_ASYNC,
  // We catch, log, and rethrow some exceptions at these intermediate levels, in case higher-level
  // handlers fail.

  ASYNC_TASK,
  REQUEST_HANDLER,
  TRACE_HANDLER,
  ALARM_HANDLER,
};

inline kj::StringPtr KJ_STRINGIFY(UncaughtExceptionSource value) {
  switch (value) {
    case UncaughtExceptionSource::INTERNAL:         return "Uncaught"_kj;
    case UncaughtExceptionSource::INTERNAL_ASYNC:   return "Uncaught (in promise)"_kj;
    case UncaughtExceptionSource::ASYNC_TASK:       return "Uncaught (async)"_kj;
    case UncaughtExceptionSource::REQUEST_HANDLER:  return "Uncaught (in response)"_kj;
    case UncaughtExceptionSource::TRACE_HANDLER:    return "Uncaught (in trace)"_kj;
    case UncaughtExceptionSource::ALARM_HANDLER:    return "Uncaught (in alarm)"_kj;
  };
  KJ_UNREACHABLE;
}

}  // namespace workerd
