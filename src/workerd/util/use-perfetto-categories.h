#pragma once

#include <workerd/util/perfetto-tracing.h>

// This header is to be imported by any translation unit (c++ file) that
// is instrumenting with perfetto traces (e.g. TRACE_EVENT). Header files
// that include instrumentation but are imported by embedders should instead
// use PERFETTO_USE_CATEGORY_FROM_NAMESPACE_SCOPED within function or block
// scopes that are to be instrumented.

#if defined(WORKERD_USE_PERFETTO)
PERFETTO_USE_CATEGORIES_FROM_NAMESPACE(workerd::traces);
#endif
