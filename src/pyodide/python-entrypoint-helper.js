// This file is a BUILTIN module that provides the actual implementation for the
// python-entrypoint.js USER module.

import { loadPyodide } from "pyodide-internal:python";
import { default as lockfile } from "pyodide-internal:generated/pyodide-lock.json";
import { default as MetadataReader } from "pyodide-internal:runtime-generated/metadata";


function initializePackageIndex(pyodide) {
  if (!lockfile.packages) {
    throw new Error(
      "Loaded pyodide lock file does not contain the expected key 'packages'.",
    );
  }
  const API = pyodide._api;
  const indexURL = `https://cdn.jsdelivr.net/pyodide/v${pyodide.version}/full/`;
  API.config.indexURL = indexURL;
  // TODO: explain here why we are setting `globalThis.location`
  globalThis.location = indexURL;
  API.lockfile_info = lockfile.info;
  API.lockfile_packages = lockfile.packages;
  API.repodata_packages = lockfile.packages;
  API.repodata_info = lockfile.info;
  API.lockfile_unvendored_stdlibs_and_test = [];

  // compute the inverted index for imports to package names
  API._import_name_to_package_name = new Map();
  for (let name of Object.keys(API.lockfile_packages)) {
    const pkg = API.lockfile_packages[name];

    for (let import_name of pkg.imports) {
      API._import_name_to_package_name.set(import_name, name);
    }

    if (pkg.package_type === "cpython_module") {
      API.lockfile_unvendored_stdlibs_and_test.push(name);
    }
  }

  API.lockfile_unvendored_stdlibs =
    API.lockfile_unvendored_stdlibs_and_test.filter((lib) => lib !== "test");

  const origLoadPackage = pyodide.loadPackage;
  pyodide.loadPackage = async function (packages, options) {
    // Must disable check integrity:
    // Subrequest integrity checking is not implemented. The integrity option
    // must be either undefined or an empty string.
    return await origLoadPackage(packages, {
      checkIntegrity: false,
      ...options,
    });
  };
}

// These packages are currently embedded inside EW and so don't need to
// be separately installed.
// If a version number is specified (i.e. non-empty), it is passed to micropip
// when running in local dev.
const EMBEDDED_PYTHON_PACKAGES = {
  "aiohttp": "3.9.3",
  "aiosignal": "1.3.1",
  "anyio": "3.7.1",
  "async_timeout": "",
  "attrs": "23.2.0",
  "certifi": "2024.2.2",
  "charset_normalizer": "3.3.2",
  "dataclasses_json": "0.6.4",
  "distro": "1.9.0",
  "fastapi": "0.109.2",
  "frozenlist": "1.4.1",
  "h11": "0.14.0",
  "httpcore": "1.0.3",
  "httpx": "0.26.0",
  "idna": "3.6",
  "jsonpatch": "1.33",
  "jsonpointer": "2.4",
  "langchain": "0.0.339",
  "langchain_community": "0.0.20",
  "langchain_core": "0.1.23",
  "langsmith": "0.0.87",
  "marshmallow": "3.20.2",
  "micropip": "0.6.0",
  "multidict": "6.0.5",
  "mypy_extensions": "1.0.0",
  "numpy": "1.26.4",
  "openai": "0.28.1",
  "packaging": "23.2",
  "pydantic": "2.6.1",
  "PyYAML": "6.0.1",
  "requests": "2.31.0",
  "setuptools": "",
  "sniffio": "1.3.0",
  "SQLAlchemy": "2.0.27",
  "starlette": "0.36.3",
  "tenacity": "8.2.3",
  "tqdm": "4.66.2",
  "typing_extensions": "4.9.0",
  "typing_inspect": "0.9.0",
  "urllib3": "2.2.0",
  "yarl": "1.9.4"
}

function addPinnedVersionToRequirement(requirement) {
  if (requirement.includes("==")) {
    // users should not be specifying version numbers themselves
    throw new Error("Python requirement must not include version number: " + requirement);
  }

  const version = EMBEDDED_PYTHON_PACKAGES[requirement];
  if (version && version.length != 0) {
    requirement += "==" + version;
  }

  return requirement;
}

async function setupPackages(pyodide) {
  // The metadata is a JSON-serialised WorkerBundle (defined in pipeline.capnp).
  const isWorkerd = MetadataReader.isWorkerd();

  const pymajor = pyodide._module._py_version_major();
  const pyminor = pyodide._module._py_version_minor();
  const site_packages = `/lib/python${pymajor}.${pyminor}/site-packages`;

  initializePackageIndex(pyodide);

  const requirements = MetadataReader.getRequirements();
  const pythonRequirements = isWorkerd ?
    requirements.map(req => addPinnedVersionToRequirement(req)) :
    requirements.filter(req => !EMBEDDED_PYTHON_PACKAGES.has(req));

  if (pythonRequirements.length > 0) {
    // Our embedded packages tarball always contains at least `micropip` so we can load it here safely.
    const micropip = pyodide.pyimport("micropip");
    await micropip.install(pythonRequirements);
  }

  // Apply patches that enable some packages to work. We don't currently list
  // out transitive dependencies so these checks are very brittle.
  // TODO: Fix this
  if (
    requirements.includes("aiohttp") ||
    requirements.includes("openai") ||
    requirements.includes("langchain")
  ) {
    const mod = await import("pyodide-internal:patches/aiohttp_fetch_patch.py");
    pyodide.FS.writeFile(
      `${site_packages}/aiohttp_fetch_patch.py`,
      new Uint8Array(mod.default),
      { canOwn: true },
    );
    pyodide.pyimport("aiohttp_fetch_patch");
  }
  if (requirements.some((req) => req.startsWith("fastapi"))) {
    const mod = await import("pyodide-internal:asgi.py");
    pyodide.FS.writeFile(
      `${site_packages}/asgi.py`,
      new Uint8Array(mod.default),
      { canOwn: true },
    );
  }
  let mainModuleName = MetadataReader.getMainModule();
  if (mainModuleName.endsWith(".py")) {
    mainModuleName = mainModuleName.slice(0, -3);
  } else {
    throw new Error("Main module needs to end with a .py file extension");
  }
  return pyodide.pyimport(mainModuleName);
}

let mainModulePromise;
function getPyodide(ctx) {
  if (mainModulePromise !== undefined) {
    return mainModulePromise;
  }
  mainModulePromise = (async function() {
    // TODO: investigate whether it is possible to run part of loadPyodide in top level scope
    // When we do it in top level scope we seem to get a broken file system.
    const pyodide = await loadPyodide(ctx);
    const mainModule = await setupPackages(pyodide);
    return { mainModule };
  })();
  return mainModulePromise;
}

export default {
  async fetch(request, env, ctx) {
    const { mainModule } = await getPyodide(ctx);
    return await mainModule.fetch.callRelaxed(request, env, ctx);
  },
  async test(ctrl, env, ctx) {
    try {
      const { mainModule } = await getPyodide(ctx);
      return await mainModule.test.callRelaxed(ctrl, env, ctx);
    } catch (e) {
      console.warn(e);
      throw e;
    }
  },
};
