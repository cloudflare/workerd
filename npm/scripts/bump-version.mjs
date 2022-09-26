// Adapted from evanw/esbuild
import fs from 'fs';

function updateVersionPackageJSON(pathToPackageJSON) {
  console.log(pathToPackageJSON);
  const json = JSON.parse(fs.readFileSync(pathToPackageJSON, 'utf8'));

  json.version = process.env.WORKERD_VERSION;

  fs.writeFileSync(pathToPackageJSON, JSON.stringify(json, null, 2) + '\n');
}

updateVersionPackageJSON(process.argv[2]);
