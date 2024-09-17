// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/string.h>

#include <cstdlib>

namespace workerd {

enum class ColorMode {
  // Always output colors to the console
  ENABLED,
  // Output colors to the console if it's a TTY
  ENABLED_IF_TTY,
  // Never output colors to the console
  DISABLED
};

// Returns whether we can output color to the console. Even if this returns `true`,
// we'll only write color codes if the output file is a TTY.
// TODO(someday): adopt more of Node.js's checks:
//  https://github.com/nodejs/node/blob/ac2a68c/lib/internal/tty.js#L106
static ColorMode permitsColor() {
  const char* forceColorValue = getenv("FORCE_COLOR");
  if (forceColorValue != nullptr) {
    auto f = kj::StringPtr(forceColorValue);
    auto enabled = f == "" || f == "1" || f == "2" || f == "3" || f == "true";
    return enabled ? ColorMode::ENABLED : ColorMode::DISABLED;
  } else {
    auto enabled = getenv("NO_COLOR") == nullptr && getenv("CI") == nullptr;
    return enabled ? ColorMode::ENABLED_IF_TTY : ColorMode::DISABLED;
  }
}

}  // namespace workerd
