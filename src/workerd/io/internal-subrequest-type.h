#pragma once

#include <kj/one-of.h>

struct GenericInternalSubrequest {};
struct DOSubrequest {};
typedef kj::OneOf<GenericInternalSubrequest, DOSubrequest> InternalSubrequestType;
