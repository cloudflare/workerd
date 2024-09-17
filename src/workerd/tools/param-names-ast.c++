// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This file includes all Worker APIs, and is used to generate a Clang AST dump for later
// lookup of parameter names for inclusion in the TS types (since RTTI doesn't include this information)
#include <workerd/api/actor-state.h>
#include <workerd/api/actor.h>
#include <workerd/api/analytics-engine.h>
#include <workerd/api/cache.h>
#include <workerd/api/crypto/crypto.h>
#include <workerd/api/encoding.h>
#include <workerd/api/events.h>
#include <workerd/api/eventsource.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/html-rewriter.h>
#include <workerd/api/hyperdrive.h>
#include <workerd/api/kv.h>
#include <workerd/api/node/node.h>
#include <workerd/api/queue.h>
#include <workerd/api/r2-admin.h>
#include <workerd/api/r2.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/sockets.h>
#include <workerd/api/sql.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/trace.h>
#include <workerd/api/urlpattern.h>
