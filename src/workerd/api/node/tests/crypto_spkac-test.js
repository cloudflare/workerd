// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import {
  Buffer,
} from 'node:buffer';

import * as assert from 'node:assert';
import { Certificate } from 'node:crypto';

const valid = Buffer.from("MIICUzCCATswggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC33FiIiiexwLe/P8DZx5HsqFlmUO7/lvJ7necJVNwqdZ3ax5jpQB0p6uxfqeOvzcN3k5V7UFb/Am+nkSNZMAZhsWzCU2Z4Pjh50QYz3f0Hour7/yIGStOLyYY3hgLK2K8TbhgjQPhdkw9+QtKlpvbL8fLgONAoGrVOFnRQGcr70iFffsm79mgZhKVMgYiHPJqJgGHvCtkGg9zMgS7p63+Q3ZWedtFS2RhMX3uCBy/mH6EOlRCNBbRmA4xxNzyf5GQaki3T+Iz9tOMjdPP+CwV2LqEdylmBuik8vrfTb3qIHLKKBAI8lXN26wWtA3kN4L7NP+cbKlCRlqctvhmylLH1AgMBAAEWE3RoaXMtaXMtYS1jaGFsbGVuZ2UwDQYJKoZIhvcNAQEEBQADggEBAIozmeW1kfDfAVwRQKileZGLRGCD7AjdHLYEe16xTBPve8Af1bDOyuWsAm4qQLYA4FAFROiKeGqxCtIErEvm87/09tCfF1My/1Uj+INjAk39DK9J9alLlTsrwSgd1lb3YlXY7TyitCmh7iXLo4pVhA2chNA3njiMq3CUpSvGbpzrESL2dv97lv590gUD988wkTDVyYsf0T8+X0Kww3AgPWGji+2f2i5/jTfD/s1lK1nqi7ZxFm0pGZoy1MJ51SCEy7Y82ajroI+5786nC02mo9ak7samca4YDZOoxN4d3tax4B/HDF5dqJSm1/31xYLDTfujCM5FkSjRc4m6hnriEkc=");

const invalid = Buffer.from("UzCCATswggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC33FiIiiexwLe/P8DZx5HsqFlmUO7/lvJ7necJVNwqdZ3ax5jpQB0p6uxfqeOvzcN3k5V7UFb/Am+nkSNZMAZhsWzCU2Z4Pjh50QYz3f0Hour7/yIGStOLyYY3hgLK2K8TbhgjQPhdkw9+QtKlpvbL8fLgONAoGrVOFnRQGcr70iFffsm79mgZhKVMgYiHPJqJgGHvCtkGg9zMgS7p63+Q3ZWedtFS2RhMX3uCBy/mH6EOlRCNBbRmA4xxNzyf5GQaki3T+Iz9tOMjdPP+CwV2LqEdylmBuik8vrfTb3qIHLKKBAI8lXN26wWtA3kN4L7NP+cbKlCRlqctvhmylLH1AgMBAAEWE3RoaXMtaXMtYS1jaGFsbGVuZ2UwDQYJKoZIhvcNAQEEBQADggEBAIozmeW1kfDfAVwRQKileZGLRGCD7AjdHLYEe16xTBPve8Af1bDOyuWsAm4qQLYA4FAFROiKeGqxCtIErEvm87/09tCfF1My/1Uj+INjAk39DK9J9alLlTsrwSgd1lb3YlXY7TyitCmh7iXLo4pVhA2chNA3njiMq3CUpSvGbpzrESL2dv97lv590gUD988wkTDVyYsf0T8+X0Kww3AgPWGji+2f2i5/jTfD/s1lK1nqi7ZxFm0pGZoy1MJ51SCEy7Y82ajroI+5786nC02mo9ak7samca4YDZOoxN4d3tax4B/HDF5dqJSm1/31xYLDTfujCM5FkSjRc4m6hnriEkc=");

const key = Buffer.from(`-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAt9xYiIonscC3vz/A2ceR
7KhZZlDu/5bye53nCVTcKnWd2seY6UAdKersX6njr83Dd5OVe1BW/wJvp5EjWTAG
YbFswlNmeD44edEGM939B6Lq+/8iBkrTi8mGN4YCytivE24YI0D4XZMPfkLSpab2
y/Hy4DjQKBq1ThZ0UBnK+9IhX37Ju/ZoGYSlTIGIhzyaiYBh7wrZBoPczIEu6et/
kN2VnnbRUtkYTF97ggcv5h+hDpUQjQW0ZgOMcTc8n+RkGpIt0/iM/bTjI3Tz/gsF
di6hHcpZgbopPL630296iByyigQCPJVzdusFrQN5DeC+zT/nGypQkZanLb4ZspSx
9QIDAQAB
-----END PUBLIC KEY-----`);

const challenge = 'this-is-a-challenge';

export const spkac = {
  test() {

    // In workerd, this following check works fine but in the internal
    // system we prevent anyone from creating a TypedArray with this
    // size... which means this check passes in workerd but fails internally,
    // but in exactly the way we want. Just going to comment this out for now.
    //
    // const buf = new Uint8Array(2 ** 31);
    // assert.throws(
    //   () => Certificate.verifySpkac(buf), {
    //     name: 'RangeError',
    //   });
    // assert.throws(
    //   () => Certificate.exportChallenge(buf), {
    //     name: 'RangeError',
    //   });
    // assert.throws(
    //   () => Certificate.exportPublicKey(buf), {
    //     name: 'RangeError',
    //   });

    // We should decide what we want to do here. The fact that we run
    // with fips enabled and SPKAC requires using md5 has the digest
    // for the signature means that verifySpkac will always fail since
    // fips mode disables the ability to use md5 here. This check also
    // fails in Node.js but there we have the options of disabling fips.
    // assert.ok(Certificate.verifySpkac(valid));
    assert.ok(!Certificate.verifySpkac(invalid));

    assert.strictEqual(
      stripLineEndings(Certificate.exportPublicKey(valid).toString('utf8')),
      stripLineEndings(key.toString('utf8'))
    );
    assert.strictEqual(Certificate.exportPublicKey(invalid).toString('utf8'), '');

    assert.strictEqual(
      Certificate.exportChallenge(valid).toString('utf8'),
      challenge
    );
    assert.strictEqual(Certificate.exportChallenge(invalid).toString('utf8'), '');

    const ab = copyArrayBuffer(key);
    assert.ok(!Certificate.verifySpkac(ab));
    assert.ok(!Certificate.verifySpkac(new Uint8Array(ab)));
    assert.ok(!Certificate.verifySpkac(new DataView(ab)));

    assert.ok(Certificate() instanceof Certificate);

    const errObj = { code: 'ERR_INVALID_ARG_TYPE' };

    [1, {}, [], Infinity, true, undefined, null].forEach((val) => {
      assert.throws(() => Certificate.verifySpkac(val), errObj);
      assert.throws(() => Certificate.exportPublicKey(val), errObj);
      assert.throws(() => Certificate.exportChallenge(val), errObj);
    });
  }
};

function stripLineEndings(obj) {
  return obj.replace(/\n/g, '');
}

function copyArrayBuffer(buf) {
  return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
}
