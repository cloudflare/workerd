// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

// Note: an explicit list here is used to avoid exposing the private export
// "initProcess"
export {
  version,
  versions,
  title,
  argv,
  argv0,
  execArgv,
  arch,
  platform,
  config,
  pid,
  ppid,
  getegid,
  getgid,
  getgroups,
  geteuid,
  getuid,
  setegid,
  setgid,
  setgroups,
  seteuid,
  setuid,
  initgroups,
  setSourceMapsEnabled,
  getSourceMapsSupport,
  nextTick,
  emitWarning,
  env,
  exit,
  getBuiltinModule,
  features,
  allowedNodeEnvironmentFlags,
  kill,
  ref,
  unref,
  chdir,
  cwd,
  umask,
  hrtime,
  uptime,
  loadEnvFile,
} from 'node-internal:internal_process';
export { default } from 'node-internal:internal_process';
