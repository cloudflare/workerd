// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The `install_dir` of a package in the lock file, which `pack_python_packages.py` records for
// each embedded package file.
type InstallDir = 'site' | 'stdlib' | 'dynlib';
