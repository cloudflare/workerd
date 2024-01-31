// This file is a BUILTIN module that provides the actual implementation for the
// python-entrypoint.js USER module.

import { loadPyodide } from "pyodide-internal:python";
import { default as lockfile } from "pyodide-internal:generated/pyodide-lock.json";
import { default as origMetadata } from "pyodide-internal:runtime-generated/current-bundle";

function initializePackageIndex(pyodide) {
  if (!lockfile.packages) {
    throw new Error(
      "Loaded pyodide lock file does not contain the expected key 'packages'."
    );
  }
  const API = pyodide._api;
  API.config.indexURL = "https://cdn.jsdelivr.net/pyodide/v0.25.0/full/";
  globalThis.location = "https://cdn.jsdelivr.net/pyodide/v0.25.0/full/";
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
const EMBEDDED_PYTHON_PACKAGES = [
  "aiohttp",
  "aiosignal",
  "anyio",
  "async_timeout",
  "attrs",
  "certifi",
  "charset_normalizer",
  "dataclasses_json",
  "distro",
  "fastapi",
  "frozenlist",
  "h11",
  "httpcore",
  "httpx",
  "idna",
  "jsonpatch",
  "jsonpointer",
  "langchain",
  "langchain_community",
  "langchain_core",
  "langsmith",
  "marshmallow",
  "micropip",
  "multidict",
  "mypy_extensions",
  "numpy",
  "openai",
  "packaging",
  "pydantic",
  "PyYAML",
  "requests",
  "setuptools",
  "sniffio",
  "SQLAlchemy",
  "starlette",
  "tenacity",
  "tqdm",
  "typing_extensions",
  "typing_inspect",
  "urllib3",
  "yarl",
];

function transformMetadata(metadata) {
  // Workerd's metadata is slightly different. We transform it here to the format used upstream.
  if (metadata.globals !== undefined) {
    return metadata;
  }

  var metadata = metadata;
  metadata.globals = [];
  for (const module of metadata.modules) {
    if (metadata.mainModule === undefined) {
      // The first module is the main module.
      metadata.mainModule = module.name;
    }

    if (module.pythonModule !== undefined) {
      metadata.globals.push({
        name: module.name,
        value: {
          pythonModule: module.pythonModule,
        },
      });
    }

    if (module.pythonRequirement !== undefined) {
      metadata.globals.push({
        name: module.name,
        value: {
          pythonRequirement: module.pythonRequirement,
        },
      });
    }
  }
  return metadata;
}

async function setupPackages(pyodide) {
  // The metadata is a JSON-serialised WorkerBundle (defined in pipeline.capnp).
  const metadata = transformMetadata(origMetadata);
  const isWorkerd = metadata.modules !== undefined;

  initializePackageIndex(pyodide);

  // Loop through globals that define Python modules in the metadata passed to our Worker. For
  // each one, save it in Pyodide's file system.
  const requirements = [];
  const pythonRequirements = [];
  const micropipRequirements = [];
  for (const { name, value } of metadata.globals) {
    if (value.pythonModule !== undefined) {
      // Support both modules names with the `.py` extension as well as without.
      const pyFilename = name.endsWith(".py") ? name : `${name}.py`;
      pyodide.FS.writeFile(`/session/${pyFilename}`, value.pythonModule, {
        canOwn: true,
      });
    }

    if (value.pythonRequirement !== undefined) {
      requirements.push(name);
      // Packages are not embedded in workerd.
      // TODO: Improve package loading in workerd.
      if (isWorkerd) {
        micropipRequirements.push(name);
        continue;
      }

      if (!EMBEDDED_PYTHON_PACKAGES.includes(name)) {
        pythonRequirements.push(name);
      }
    }
  }

  if (pythonRequirements.length > 0) {
    await pyodide.loadPackage(pythonRequirements);
  }

  if (micropipRequirements.length > 0) {
    // Micropip and ssl packages are pre-loaded via the packages tarball. This means
    // we should be able to load micropip directly now.

    const micropip = pyodide.pyimport("micropip");
    await micropip.install(micropipRequirements);
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
      "/lib/python3.11/site-packages/aiohttp_fetch_patch.py",
      new Uint8Array(mod.default),
      { canOwn: true }
    );
    pyodide.pyimport("aiohttp_fetch_patch");
  }
  if (requirements.includes("fastapi")) {
    const mod = await import("pyodide-internal:asgi.py");
    pyodide.FS.writeFile(
      "/lib/python3.11/site-packages/asgi.py",
      new Uint8Array(mod.default),
      { canOwn: true }
    );
  }

  // The main module can have a `.py` extension, strip it if it exists.
  const mainName = metadata.mainModule;
  const mainModule = mainName.endsWith(".py") ? mainName.slice(0, -3) : mainName;

  return pyodide.pyimport(mainModule);
}

export default {
  async fetch(request, env) {
    const pyodide = await loadPyodide();
    const mainModule = await setupPackages(pyodide);
    return await mainModule.fetch(request);
  },
  async test() {
    try {
      const pyodide = await loadPyodide();
      const mainModule = await setupPackages(pyodide);
      return await mainModule.test();
    } catch (e) {
      console.warn(e);
    }
  },
};
