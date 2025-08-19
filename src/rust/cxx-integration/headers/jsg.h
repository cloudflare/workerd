#pragma once

// Generic JSG header wrapper for Rust CXX bridge integration
// This provides the JSG_STRUCT macro and other JSG functionality
// for Rust modules that need to integrate with the JavaScript Glue layer
//
// We need this file because workerd-cxx adds 'include "jsg.h"' whenever JsgStruct
// derive macro is used. workerd-cxx adds jsg.h but not workerd/jsg/jsg.h to avoid
// fixing the include path, and to avoid the extra maintenance burden of having to
// update the include path in multiple places.
#include <workerd/jsg/jsg.h>
