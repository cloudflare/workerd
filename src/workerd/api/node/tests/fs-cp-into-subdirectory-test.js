import { cpSync, mkdirSync, writeFileSync } from 'node:fs';
import { throws } from 'node:assert';

export const FsCpIntoSubdirectory = {
  test() {
    mkdirSync(new URL('file:///tmp/src'), { recursive: true });
    writeFileSync(new URL('file:///tmp/src/file.txt'), 'test');

    throws(
      () =>
        cpSync(new URL('file:///tmp/src'), new URL('file:///tmp/src/dest'), {
          recursive: true,
        }),
      {
        code: 'ERR_FS_CP_EINVAL',
      }
    );
  },
};
