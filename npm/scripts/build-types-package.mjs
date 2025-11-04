import path from "path";
import fs from "fs";

function buildTypesPackage() {
  const packageJsonPath = path.join("npm", "workers-types", "package.json");
  const packageJson = JSON.parse(fs.readFileSync(packageJsonPath, "utf8"));
  packageJson.version = process.env.WORKERD_VERSION;
  fs.writeFileSync(
    packageJsonPath,
    JSON.stringify(packageJson, null, 2) + "\n"
  );
}

buildTypesPackage();
