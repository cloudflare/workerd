import { default as yazl} from 'yazl';
import {
  createWriteStream,
  readFileSync,
  readdirSync,
} from 'node:fs';

export default {
  fetch(request) {

    const { promise, resolve, reject } = Promise.withResolvers();

    const zipfile = new yazl.ZipFile();

    const entries = readdirSync("/bundle");
    for (const entry of entries) {
      zipfile.addFile(`/bundle/${entry}`, entry);
    }

    zipfile.outputStream.pipe(createWriteStream("/tmp/output.zip"))
      .on("close", function() {
        const buffer = readFileSync("/tmp/output.zip");
        resolve(new Response(buffer));
    });

    zipfile.end();

    return promise;
  }
};
