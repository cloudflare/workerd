import { loadPyodide } from "pyodide:python";
import { getMetadata } from "pyodide:current-bundle";
import { lockFile } from "pyodide:package-lock.json";
import { getPatches } from "pyodide:patches";
import embeddedPackages from "pyodide:embedded_packages";

function initializePackageIndex(pyodide, lockfile) {
  if (!lockfile.packages) {
    throw new Error(
      "Loaded pyodide lock file does not contain the expected key 'packages'.",
    );
  }
  const API = pyodide._api;
  API.config.indexURL = "https://cdn.jsdelivr.net/pyodide/v0.25.0a2/full/";
  globalThis.location = "https://cdn.jsdelivr.net/pyodide/v0.25.0a2/full/";
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

// These packages are currently embedded inside workerd and so don't need to
// be separately installed.
const EMBEDDED_PYTHON_PACKAGES = [
  "tqdm",
  "openai",
  "numpy",
  "SQLAlchemy",
  "typing_extensions",
  "PyYAML",
  "aiohttp",
  "aiosignal",
  "frozenlist",
  "async_timeout",
  "attrs",
  "six",
  "charset_normalizer",
  "multidict",
  "yarl",
  "idna",
  "pydantic",
  "certifi",
  "langchain",
  "anyio",
  "tenacity",
  "langsmith",
  "dataclasses_json",
  "jsonpatch",
  "requests",
  "sniffio",
  "marshmallow",
  "urllib3",
  "typing_inspect",
  "jsonpointer",
  "mypy_extensions",
  "micropip",
  "packaging"
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
          pythonModule: module.pythonModule
        }
      })
    }

    if (module.pythonRequirement !== undefined) {
      metadata.globals.push({
        name: module.name,
        value: {
          pythonRequirement: module.pythonRequirement
        }
      })
    }
  }
  return metadata;
}

export default {
  async fetch(request, env) {
    // The metadata is a JSON-serialised WorkerBundle (defined in pipeline.capnp).
    const metadata = transformMetadata(getMetadata());

    const pyodide = await loadPyodide();
    initializePackageIndex(pyodide, lockFile);

    // Loop through globals that define Python modules in the metadata passed to our Worker. For
    // each one, save it in Pyodide's file system.
    let hasRequirements = false;
    const pythonRequirements = [];
    const micropipRequirements = [];
    for (const { name, value } of metadata.globals) {
      if (value.pythonModule !== undefined) {
        pyodide.FS.writeFile(`/session/${name}.py`, value.pythonModule, {
          canOwn: true,
        });
      }

      if (value.pythonRequirement !== undefined) {
        hasRequirements = true;
        if (!EMBEDDED_PYTHON_PACKAGES.includes(name)) {
          pythonRequirements.push(name);
        }
      }
    }

    if (hasRequirements) {
      const name = "pyodide_packages_unzipped_0.2.tar";
      const path = `/lib/python3.11/site-packages/${name}`;
      pyodide.FS.writeFile(path, new Uint8Array(embeddedPackages), {
        encoding: 'binary',
      });

      pyodide.runPython(`
      import tarfile
      import os

      tar_file_path = "${path}"
      containing_dir = os.path.dirname(tar_file_path)
      with tarfile.open(tar_file_path, 'r') as tar:
        tar.extractall(containing_dir)
      `)

      const micropip = pyodide.pyimport("micropip");
      if (micropipRequirements.length > 0) {
        // Micropip and ssl packages are contained in the tarball which is extracted above. This means
        // we should be able to load micropip directly now.
        await micropip.install(micropipRequirements);
      }

      // Apply patches that enable some packages to work.
      const patches = getPatches();
      // TODO(EW-8055): Why does micropip.list not work?
      if (JSON.parse(micropip.freeze())["packages"]["aiohttp"] !== undefined) {
        pyodide.runPython(patches["aiohttp_fetch_patch.py"]);
      }
    }


    await pyodide.loadPackage(pythonRequirements);

    return await pyodide.pyimport(metadata.mainModule).fetch(request);
  },
};
