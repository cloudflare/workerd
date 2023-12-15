import { loadPyodide } from "pyodide:python";
import { getMetadata } from "pyodide:current-bundle";
import { lockFile } from "pyodide:package-lock.json";

function initializePackageIndex(pyodide, lockfile) {
  if (!lockfile.packages) {
    throw new Error(
      "Loaded pyodide lock file does not contain the expected key 'packages'.",
    );
  }
  const API = pyodide._api;
  API.config.indexURL = "https://cdn.jsdelivr.net/pyodide/v0.25.0a1/full/";
  globalThis.location = "https://cdn.jsdelivr.net/pyodide/v0.25.0a1/full/";
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

export default {
  async fetch(request, env) {
    // The metadata is a JSON-serialised WorkerBundle (defined in pipeline.capnp).
    const metadata = getMetadata();

    const pyodide = await loadPyodide();
    initializePackageIndex(pyodide, lockFile);
    // Loop through globals that define Python modules in the metadata passed to our Worker. For
    // each one, save it in Pyodide's file system.
    const pythonRequirements = [];
    for (const { name, value } of metadata.globals) {
      if (value.pythonModule !== undefined) {
        pyodide.FS.writeFile(`/session/${name}.py`, value.pythonModule, {
          canOwn: true,
        });
      }

      if (value.pythonRequirement !== undefined) {
        pythonRequirements.push(name);
      }
    }
    await pyodide.loadPackage(pythonRequirements);

    const result = await pyodide.pyimport(metadata.mainModule).fetch(request);

    return result;
  },
};
