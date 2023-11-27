# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0x9877d11df1c5f4ab;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd");
$Cxx.allowCancellation;

const supportedCompatibilityDate :Text = embed "/trimmed-supported-compatibility-date.txt";
# Newest compatibility date that can safely be set using code compiled from this repo. Trying to
# run a Worker with a newer compatibility date than this will fail.
#
# This should be updated to the current date on a regular basis. The reason this exists is so that
# if you build an old version of the code and try to run a new Worker with it, you get an
# appropriate error, rather than have the code run with the wrong compatibility mode.
#
# Note that the production Cloudflare Workers upload API always accepts any date up to the current
# date regardless of this constant, on the assumption that Cloudflare is always running the latest
# version of the code. This constant is more to protect users who are self-hosting the runtime and
# could be running an older version.
