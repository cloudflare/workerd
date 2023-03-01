// Adapted from github.com/evanw/esbuild
// Original copyright and license:
//     Copyright (c) 2020 Evan Wallace
//     MIT License
//
//     Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//     The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
import fs from "fs";
import os from "os";
import path from "path";

declare const WORKERD_VERSION: string;

export const knownPackages: Record<string, string> = {
  "darwin arm64 LE": "@cloudflare/workerd-darwin-arm64",
  "darwin x64 LE": "@cloudflare/workerd-darwin-64",
  "linux arm64 LE": "@cloudflare/workerd-linux-arm64",
  "linux x64 LE": "@cloudflare/workerd-linux-64",
  "win32 x64 LE": "@cloudflare/workerd-linux-64",
};

export function pkgAndSubpathForCurrentPlatform(): {
  pkg: string;
  subpath: string;
} {
  let pkg: string;
  let subpath: string;
  let platformKey = `${process.platform} ${os.arch()} ${os.endianness()}`;

  if (platformKey in knownPackages) {
    pkg = knownPackages[platformKey];
    subpath = "bin/workerd";
  } else {
    throw new Error(`Unsupported platform: ${platformKey}`);
  }

  return { pkg, subpath };
}

function pkgForSomeOtherPlatform(): string | null {
  const libMain = require.resolve("workerd");
  const nodeModulesDirectory = path.dirname(
    path.dirname(path.dirname(libMain))
  );

  if (path.basename(nodeModulesDirectory) === "node_modules") {
    for (const unixKey in knownPackages) {
      try {
        const pkg = knownPackages[unixKey];
        if (fs.existsSync(path.join(nodeModulesDirectory, pkg))) return pkg;
      } catch {}
    }
  }

  return null;
}

export function downloadedBinPath(pkg: string, subpath: string): string {
  const libDir = path.dirname(require.resolve("workerd"));
  return path.join(libDir, `downloaded-${pkg.replace("/", "-")}-${path.basename(subpath)}`);
}

export function generateBinPath(): { binPath: string } {
  const { pkg, subpath } = pkgAndSubpathForCurrentPlatform();
  let binPath: string;

  try {
    // First check for the binary package from our "optionalDependencies". This
    // package should have been installed alongside this package at install time.
    binPath = require.resolve(`${pkg}/${subpath}`);
  } catch (e) {
    // If that didn't work, then someone probably installed workerd with the
    // "--no-optional" flag. Our install script attempts to compensate for this
    // by manually downloading the package instead. Check for that next.
    binPath = downloadedBinPath(pkg, subpath);
    if (!fs.existsSync(binPath)) {
      // If that didn't work too, check to see whether the package is even there
      // at all. It may not be (for a few different reasons).
      try {
        require.resolve(pkg);
      } catch {
        // If we can't find the package for this platform, then it's possible
        // that someone installed this for some other platform and is trying
        // to use it without reinstalling. That won't work of course, but
        // people do this all the time with systems like Docker. Try to be
        // helpful in that case.
        const otherPkg = pkgForSomeOtherPlatform();
        if (otherPkg) {
          throw new Error(`
You installed workerd on another platform than the one you're currently using.
This won't work because workerd is written with native code and needs to
install a platform-specific binary executable.

Specifically the "${otherPkg}" package is present but this platform
needs the "${pkg}" package instead. People often get into this
situation by installing workerd on macOS and copying "node_modules"
into a Docker image that runs Linux.

If you are installing with npm, you can try not copying the "node_modules"
directory when you copy the files over, and running "npm ci" or "npm install"
on the destination platform after the copy. Or you could consider using yarn
instead which has built-in support for installing a package on multiple
platforms simultaneously.

If you are installing with yarn, you can try listing both this platform and the
other platform in your ".yarnrc.yml" file using the "supportedArchitectures"
feature: https://yarnpkg.com/configuration/yarnrc/#supportedArchitectures
Keep in mind that this means multiple copies of workerd will be present.
`);
        }

        // If that didn't work too, then maybe someone installed workerd with
        // both the "--no-optional" and the "--ignore-scripts" flags. The fix
        // for this is to just not do that. We don't attempt to handle this
        // case at all.
        //
        // In that case we try to have a nice error message if we think we know
        // what's happening. Otherwise we just rethrow the original error message.
        throw new Error(`The package "${pkg}" could not be found, and is needed by workerd.

If you are installing workerd with npm, make sure that you don't specify the
"--no-optional" flag. The "optionalDependencies" package.json feature is used
by workerd to install the correct binary executable for your current platform.`);
      }
      throw e;
    }
  }

  // The workerd binary executable can't be used in Yarn 2 in PnP mode because
  // it's inside a virtual file system and the OS needs it in the real file
  // system. So we need to copy the file out of the virtual file system into
  // the real file system.
  //
  // You might think that we could use "preferUnplugged: true" in each of the
  // platform-specific packages for this instead, since that tells Yarn to not
  // use the virtual file system for those packages. This is not done because:
  //
  // * Really early versions of Yarn don't support "preferUnplugged", so package
  //   installation would break on those Yarn versions if we did this.
  //
  // * Earlier Yarn versions always installed all optional dependencies for all
  //   platforms even though most of them are incompatible. To minimize file
  //   system space, we want these useless packages to take up as little space
  //   as possible so they should remain unzipped inside their ".zip" files.
  //
  //   We have to explicitly pass "preferUnplugged: false" instead of leaving
  //   it up to Yarn's default behavior because Yarn's heuristics otherwise
  //   automatically unzip packages containing ".exe" files, and we don't want
  //   our Windows-specific packages to be unzipped either.
  //
  let pnpapi: any;
  try {
    pnpapi = require("pnpapi");
  } catch (e) {}
  if (pnpapi) {
    const root = pnpapi.getPackageInformation(pnpapi.topLevel).packageLocation;
    const binTargetPath = path.join(
      root,
      "node_modules",
      ".cache",
      "workerd",
      `pnpapi-${pkg.replace("/", "-")}-${WORKERD_VERSION}-${path.basename(subpath)}`
    );
    if (!fs.existsSync(binTargetPath)) {
      fs.mkdirSync(path.dirname(binTargetPath), { recursive: true });
      fs.copyFileSync(binPath, binTargetPath);
      fs.chmodSync(binTargetPath, 0o755);
    }
    return { binPath: binTargetPath };
  }

  return { binPath };
}
