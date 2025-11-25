#pragma once

#include <kj/string.h>
#include <workerd/io/trace.capnp.h>

namespace workerd {

// Format: SPAN_OP(name, enum_type, description)
// - name: string name of the span (e.g., "fetch")
// - enum_type: capnp enum value (e.g., rpc::UserSpanOperationType::FETCH)
// - description: documentation, forces authors to document spans
#define USER_SPAN_OPERATIONS(SPAN_OP) \
  SPAN_OP("unknown", rpc::UserSpanOperationType::UNKNOWN, "Unknown span operation") \
  SPAN_OP("fetch", rpc::UserSpanOperationType::FETCH, "Outbound fetch subrequest") \
  SPAN_OP("cache_match", rpc::UserSpanOperationType::CACHE_MATCH, "Cache match operation")

class UserSpanOperation {
 public:
  using Type = rpc::UserSpanOperationType;

  constexpr UserSpanOperation(Type type): type_(type) {}
  constexpr UserSpanOperation(kj::LiteralStringConst name): type_(fromString(name)) {}

  UserSpanOperation(const UserSpanOperation&) = default;
  UserSpanOperation(UserSpanOperation&&) = default;
  UserSpanOperation& operator=(const UserSpanOperation&) = default;
  UserSpanOperation& operator=(UserSpanOperation&&) = default;

  kj::StringPtr asPtr() const { return toString(type_); }
  Type type() const { return type_; }

  static kj::Maybe<UserSpanOperation> tryFromString(kj::StringPtr name);

 private:
  Type type_;

  static constexpr kj::StringPtr toString(Type type) {
    switch (type) {
#define SPAN_OP(name, enum_type, desc) case enum_type: return name##_kj;
      USER_SPAN_OPERATIONS(SPAN_OP)
#undef SPAN_OP
    }
    return ""_kj;
  }

  static constexpr Type fromString(kj::LiteralStringConst name) {
#define SPAN_OP(str_name, enum_type, desc) \
    if (name == str_name##_kjc) return enum_type;
    USER_SPAN_OPERATIONS(SPAN_OP)
#undef SPAN_OP
    return rpc::UserSpanOperationType::UNKNOWN;
  }
};

}  // namespace workerd
