import {
  strictEqual,
  deepStrictEqual,
  notStrictEqual,
  rejects,
  throws,
} from 'node:assert';

export const apiFormDataParse = {
  async test(ctrl, env) {
    const INPUT = `---
Content-Disposition: form-data; name="field0"

part0
---
Content-Disposition: form-data; name="field1"

part1
---
Content-Disposition: form-data; name="field0"

part2
---
Content-Disposition: form-data; name="field1"

part3
-----`;

    const req = new Request('https://example.com', {
      method: 'POST',
      body: INPUT,
      headers: {
        'content-type': 'multipart/form-data; Boundary="-"',
      },
    });

    const formData = await req.formData();

    deepStrictEqual(formData.getAll('field0'), ['part0', 'part2']);
    deepStrictEqual(formData.getAll('field1'), ['part1', 'part3']);
  },
};

export const invalidFormdataContentDisposition = {
  async test() {
    const INPUT = `--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: foobar
Content-Type: application/octet-stream

foo-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a--
`;

    const req = new Request('https://example.org', {
      method: 'POST',
      body: INPUT,
      headers: {
        'content-type':
          'multipart/form-data;boundary=2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a',
      },
    });

    try {
      await req.formData();
      throw new Error('Parsing the form data should have thrown');
    } catch (err) {
      strictEqual(
        err.message,
        'Content-Disposition header for FormData part must ' +
          'have the value "form-data", possibly followed by ' +
          'parameters. Got: "foobar"'
      );
    }
  },
};

export const invalidFormData = {
  async test() {
    const form = new FormData();
    form.set('foo', new File(['foo-content'], 'foo.txt\\'));
    try {
      new Request('http://example.org', {
        method: 'POST',
        body: form,
      });
      throw new Error('should have thrown');
    } catch (err) {
      strictEqual(err.message, "Name or filename can't end with backslash");
    }
  },
};

export const formDataWithFilesBlobs = {
  async test() {
    const INPUT = `--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="foo"; filename="foo.txt"
Content-Type: application/octet-stream

foo-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="bar"; filename="bar-renamed.txt"
Content-Type: application/octet-stream

bar1-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="bar"; filename="bar2.txt"
Content-Type: text/bary

bar2-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="baz"; filename="baz"
Content-Type: text/bazzy

baz-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="qux"; filename="qux%0A%22\\\\.txt"
Content-Type: application/octet-stream

qux-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a--
`;

    const req = new Request('https://example.org', {
      method: 'POST',
      body: INPUT,
      headers: {
        'content-type':
          'multipart/form-data;boundary=2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a',
      },
    });

    async function assertFile(file, name, type, content) {
      if (!(file instanceof File)) {
        throw new Error('not a File: ' + file);
      }

      strictEqual(name, file.name);
      strictEqual(type, file.type);
      strictEqual(content, await file.text());
    }

    const form = await req.formData();
    await assertFile(
      form.get('foo'),
      'foo.txt',
      'application/octet-stream',
      'foo-content'
    );
    await assertFile(
      form.getAll('bar')[0],
      'bar-renamed.txt',
      'application/octet-stream',
      'bar1-content'
    );
    await assertFile(
      form.getAll('bar')[1],
      'bar2.txt',
      'text/bary',
      'bar2-content'
    );
    await assertFile(form.get('baz'), 'baz', 'text/bazzy', 'baz-content');
    await assertFile(
      form.get('qux'),
      'qux%0A%22\\.txt',
      'application/octet-stream',
      'qux-content'
    );
  },
};

export const sendFilesInFormdata = {
  async test() {
    const INPUT = `--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="foo"; filename="foo.txt"
Content-Type: application/octet-stream

foo-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="bar"; filename="bar-renamed.txt"
Content-Type: application/octet-stream

bar1-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="bar"; filename="bar2.txt"
Content-Type: text/bary

bar2-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="baz"; filename="baz"
Content-Type: text/bazzy

baz-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a
Content-Disposition: form-data; name="qux"; filename="qux%0A%22\\.txt"
Content-Type: application/octet-stream

qux-content
--2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a--
`;

    const req = new Request('https://example.org', {
      method: 'POST',
      body: INPUT,
      headers: {
        'content-type':
          'multipart/form-data;boundary=2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a',
      },
    });

    const form = await req.formData();

    form.set('foo', new File(['foo-content'], 'foo.txt'));
    form.append(
      'bar',
      new File(['bar1-content'], 'bar1.txt'),
      'bar-renamed.txt'
    );
    form.append(
      'bar',
      new File(['bar2-content'], 'bar2.txt', { type: 'text/bary' })
    );
    form.append('baz', new Blob(['baz-content'], { type: 'text/bazzy' }));
    form.set('qux', new Blob(['qux-content']), 'qux\n"\\.txt');

    if (!(form.get('foo') instanceof File)) {
      throw new Error('expected file');
    }
    if (form.get('foo').name != 'foo.txt') {
      throw new Error('expected file name foo.txt');
    }
    if (!(form.getAll('bar')[1] instanceof File)) {
      throw new Error('expected files');
    }
  },
};

async function parseFormData(contentType, text) {
  const req = new Request('http://example.org', {
    method: 'POST',
    body: text,
    headers: {
      'content-type': contentType,
    },
  });
  return await req.formData();
}

export const testFormDataParser = {
  async test() {
    const successCases = [
      {
        // No parts. Note that Chrome throws a TypeError on this input, but it'll generate output that
        // looks like this if you ask it to serialize an empty form.
        contentType: 'multipart/form-data; boundary="+"',
        body: '--+--',
        expected: '',
        comment: 'Empty form is okay',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: [
          // CRLF after boundary, CRLFCRLF after header, CRLF after message
          '--+\r\n',
          'Content-Disposition: form-data; name="field0"\r\n',
          '\r\n',
          'part0\r\n',

          // LF after boundary, CRLFLF after header, tabs in header, LF after message
          '--+\n',
          'CONTENT-DISPOSITION:\tform-data\t;\tname="field1"\r\n',
          '\n',
          'part1\n',

          // LFCRLF after header, no OWS in header, Content-Type header above disposition
          '--+\r\n',
          'content-disposition:form-data;name="field0"\n',
          '\r\n',
          'part2\r\n',

          // LFCRLF after header
          '--+\r\n',
          'CoNTent-dIsposiTIOn: form-data; name="field1"\n',
          '\r\n',
          'part3\r\n',

          '--+--',
        ].join(''),
        expected: 'field0=part0,field1=part1,field0=part2,field1=part3',
        comment: 'Mixed CRLF and LF, case-insensitivity of header name',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: [
          // CRLFCRLF after header, empty message with CRLF
          '--+\r\n',
          'Content-Disposition: form-data; name="empties"\r\n',
          '\r\n',
          '\r\n',

          // CRLFCRLF after header, empty message with LF
          '--+\r\n',
          'Content-Disposition: form-data; name="empties"\r\n',
          '\r\n',
          '\n',

          // CRLFLF after header, empty message with CRLF
          '--+\r\n',
          'Content-Disposition: form-data; name="empties"\r\n',
          '\n',
          '\r\n',

          // CRLFLF after header, empty message with LF
          '--+\r\n',
          'Content-Disposition: form-data; name="empties"\r\n',
          '\n',
          '\n',

          // LFCRLF after header, empty message with CRLF
          '--+\r\n',
          'Content-Disposition: form-data; name="empties"\n',
          '\r\n',
          '\r\n',

          // LFCRLF after header, empty message with LF
          '--+\r\n',
          'Content-Disposition: form-data; name="empties"\n',
          '\r\n',
          '\n',

          // LFLF after header, empty message with CRLF
          '--+\r\n',
          'Content-Disposition: form-data; name="empties"\n',
          '\n',
          '\r\n',

          // LFLF after header, empty message with LF
          '--+\r\n',
          'Content-Disposition: form-data; name="empties"\n',
          '\n',
          '\n',

          '--+--',
        ].join(''),
        expected:
          'empties=,empties=,empties=,empties=,empties=,empties=,empties=,empties=',
        comment: 'Mixed CRLF and LF with empty messages',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: [
          '--+\r\n',
          'Content-Type: text/plain; charset=utf-8\r\n',
          'Content-Disposition: form-data; name="field0"\r\n',
          '\r\n',
          'part0\r\n',

          '--+\r\n',
          'Content-Disposition: form-data; name="field1"\r\n',
          'Content-Type: text/plain; charset=utf-8\r\n',
          '\r\n',
          'part1\r\n',

          '--+--',
        ].join(''),
        expected: 'field0=part0,field1=part1',
        comment: 'Content-Type header should be okay',
      },
      {
        contentType: 'application/x-www-form-urlencoded',
        body: [
          'field0=part0',
          'field1=part1',
          'field0=part2',
          'field1=part3',
        ].join('&'),
        expected: 'field0=part0,field1=part1,field0=part2,field1=part3',
        comment: 'Basic application/x-www-form-urlencoded parse works',
      },
      {
        contentType: 'application/x-www-form-urlencoded',
        body: ['field0=data+with+an+%26+in+it'].join('&'),
        expected: 'field0=data with an & in it',
        comment:
          'application/x-www-form-urlencoded data gets percent-and-plus-decoded',
      },
      {
        contentType: 'application/x-www-form-urlencoded',
        body: ['', '=', 'field1', '=part2', 'field1='].join('&'),
        expected: '=,field1=,=part2,field1=',
        comment:
          'application/x-www-form-urlencoded data with awkward &, = placement',
      },
    ];

    for (let i = 0; i < successCases.length; ++i) {
      const c = successCases[i];
      const fd = await parseFormData(c.contentType, c.body);
      const actual = [];
      for (let [k, v] of fd) {
        actual.push(`${k}=${v}`);
      }
      strictEqual(actual.join(','), c.expected);
    }

    let failureCases = [
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: '',
        comment: 'Empty body throws',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: '--asdf--',
        comment: 'Bad boundary throws',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: '--+',
        comment: 'Non-terminal boundary at end throws',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: [
          '--+\r\n',
          'Content-Disposition: form-data; name="field0"\r\n',
          '\r\n',
          'part0\r\n',
          '-+--',
        ].join(''),
        comment: 'Bad terminal delimiter',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: [
          '--+\r\n',
          'Bad-Content-Disposition: form-data; name="field0"\r\n',
          '\r\n',
          'part0\r\n',
          '--+--',
        ].join(''),
        comment: 'Bad Content-Disposition header',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: [
          '--+\r\n',
          'Content-Disposition-Bad: form-data; name="field0"\r\n',
          '\r\n',
          'part0\r\n',
          '--+--',
        ].join(''),
        comment: 'Bad Content-Disposition header',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: [
          '--+\r\n',
          'Content-Disposition: form-data; name="field0"\r\n',
          '\r',
          'part0\r\n',
          '--+--',
        ].join(''),
        comment: 'No header termination CRLFCRLF',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: [
          '--+\r\n',
          'Content-Disposition: form-data; name="field0"\r\n',
          '\r\n',
          'part0\r\n',
        ].join(''),
        comment: 'No subsequent boundary string',
      },
      {
        contentType: 'multipart/form-data; boundary="+"',
        body: [
          '--+\r\n',
          'Content-Disposition: form-data; name="field0"\r\n',
          '\r\n',
          'part0\r\n',
          '--+\r--',
        ].join(''),
        comment: "Boundary was not succeeded by CRLF, LF, or '--'",
      },
      {
        contentType: 'multipart/form-data; boundary=',
        body: '----',
        comment: 'Empty boundary parameter in content-type',
      },
      {
        contentType: 'application/x-www-form-urlencoded; charset=big5',
        body: '--+--',
        comment: 'Unsupported charset',
      },
    ];

    for (let i = 0; i < failureCases.length; ++i) {
      const c = failureCases[i];
      await rejects(() => parseFormData(c.contentType, c.body));
    }
  },
};

export const testFormDataSerializer = {
  async test() {
    // Test the serializer by making a round trip through our serializer and parser.

    const expected = new FormData();
    expected.append('field0', 'part0');
    expected.append('field1', 'part1');
    expected.append('field0', 'part2');
    expected.append('field1', 'part3');
    expected.append('field-with-a-"-in-it', 'part4');

    // Serialize the FormData.
    const response = new Response(expected);

    // Parse it back.
    // This regex assumes an unquoted boundary, which is true for our serializer.
    const boundary = /boundary=(.+)$/.exec(
      response.headers.get('Content-Type')
    )[1];
    const actual = await parseFormData(
      `multipart/form-data; boundary="${boundary}"`,
      await response.text()
    );

    const expectedData = [
      'field0=part0',
      'field1=part1',
      'field0=part2',
      'field1=part3',
      'field-with-a-%22-in-it=part4',
    ];
    const actualData = [];
    for (let [k, v] of actual) {
      actualData.push(`${k}=${v}`);
    }

    strictEqual('' + actualData.join(','), '' + expectedData.join(','));
  },
};

export const testFormDataSet = {
  test() {
    const fd = new FormData();
    fd.append('foo', '0');
    fd.append('foo', '1');
    fd.append('foo', '2');
    fd.append('foo', '3');
    fd.append('foo', '4');
    fd.append('foo', '5');
    fd.set('foo', '6');

    strictEqual('' + fd.getAll('foo'), '6');
  },
};

export const testFormDataIterators = {
  test() {
    const fd = new FormData();
    const entry = fd.entries();
    const key = fd.keys();
    const value = fd.values();
    strictEqual(entry.next().value, undefined);
    strictEqual(key.next().value, undefined);
    strictEqual(value.next().value, undefined);

    fd.append('key', '0');
    fd.append('key', '1');
    strictEqual('' + entry.next().value, 'key,0');
    strictEqual('' + key.next().value, 'key');
    strictEqual('' + value.next().value, '0');

    fd.delete('key');
    strictEqual(entry.next().value, undefined);
    strictEqual(key.next().value, undefined);
    strictEqual(value.next().value, undefined);
  },
};

export const testFormDataForeach = {
  test() {
    const fd = new FormData();

    fd.forEach(function (v, k, t) {
      throw new Error('should not be called on empty array');
    });

    let foreachOutput = [];
    fd.append('key1', 'value1');
    fd.append('key2', 'value2');

    let i = 0;
    fd.forEach(function (value, key, captureFd) {
      notStrictEqual(value, '3'); // if this is true, then the test is useless
      // updating the headers should affect them immediately when not called through forEach
      captureFd.set(key, '3');
      strictEqual(captureFd.get(key), '3');
      // updating the headers should not affect `value`
      notStrictEqual(value, '3');
      foreachOutput.push(`${key}=${value}`);

      captureFd.append('some-key', '4');
      // console.log("appended");
      i += 1;
    });

    // appending keys within the loop should call the callback on the new items
    strictEqual(i, 4);
    strictEqual(
      '' + foreachOutput.join('&'),
      'key1=value1&key2=value2&some-key=4&some-key=4'
    );
    // `capture_headers.set` should affect the outer headers object
    strictEqual(fd.get('key1'), '3');
    strictEqual(fd.get('key2'), '3');
    // `capture_headers.append` should affect the outer object
    deepStrictEqual(fd.getAll('some-key'), ['3', '4']);

    throws(() => fd.forEach());
    throws(() => fd.forEach(1));

    // `this` can be overriden by setting the second argument
    fd.forEach(function () {
      // NOTE: can't use `assert_equals` because `this` has type `object` which apparently it doesn't like
      strictEqual(this, 1);
    }, 1);

    throws(() => {
      fd.forEach(function () {
        throw new Error('boo');
      });
    });

    // forEach should not move the value
    fd.set('key1', 'a');
    fd.forEach(() => {});
    strictEqual(fd.get('key1'), 'a');
  },
};

export const w3cTestFormDataAppend = {
  test() {
    function test_formdata(creator, verifier, description) {
      let result = [];
      for (let [k, v] of creator()) {
        result.push(`${k}=${v}`);
      }
      verifier(result.join(','));
    }

    test_formdata(
      function () {
        var fd = new FormData();
        fd.append('name', new String('value'));
        return fd;
      },
      function (data) {
        strictEqual(data, 'name=value');
      },
      'Passing a String object to FormData.append should work.'
    );

    strictEqual(create_formdata(['key', 'value1']).get('key'), 'value1');
    strictEqual(
      create_formdata(['key', 'value2'], ['key', 'value1']).get('key'),
      'value2'
    );
    strictEqual(create_formdata(['key', undefined]).get('key'), 'undefined');
    strictEqual(
      create_formdata(['key', undefined], ['key', 'value1']).get('key'),
      'undefined'
    );
    strictEqual(create_formdata(['key', null]).get('key'), 'null');
    strictEqual(
      create_formdata(['key', null], ['key', 'value1']).get('key'),
      'null'
    );

    function create_formdata() {
      var fd = new FormData();
      for (var i = 0; i < arguments.length; i++) {
        fd.append.apply(fd, arguments[i]);
      }
      return fd;
    }
  },
};

export const w3cTestFormDataBlob = {
  test() {
    function create_formdata() {
      var fd = new FormData();
      for (var i = 0; i < arguments.length; i++) {
        fd.append.apply(fd, arguments[i]);
      }
      return fd;
    }

    throws(() => create_formdata('a', 'b', 'c'));
  },
};

export const w3cTestFormDataDelete = {
  test() {
    {
      var fd = create_formdata(['key', 'value1'], ['key', 'value2']);
      fd.delete('key');
      strictEqual(fd.get('key'), null);
    }

    {
      var fd = create_formdata(['key', 'value1'], ['key', 'value2']);
      fd.delete('nil');
      strictEqual(fd.get('key'), 'value1');
    }

    {
      var fd = create_formdata(['key1', 'value1'], ['key2', 'value2']);
      fd.delete('key1');
      strictEqual(fd.get('key1'), null);
      strictEqual(fd.get('key2'), 'value2');
    }

    function create_formdata() {
      var fd = new FormData();
      for (var i = 0; i < arguments.length; i++) {
        fd.append.apply(fd, arguments[i]);
      }
      return fd;
    }
  },
};

export const w3cTestFormDataForeach = {
  test() {
    var fd = new FormData();
    fd.append('n1', 'v1');
    fd.append('n2', 'v2');
    fd.append('n3', 'v3');
    fd.append('n1', 'v4');
    fd.append('n2', 'v5');
    fd.append('n3', 'v6');
    fd.delete('n2');
    var expected_keys = ['n1', 'n3', 'n1', 'n3'];
    var expected_values = ['v1', 'v3', 'v4', 'v6'];
    // TODO(soon): Test with this File object.
    //var file = new File(['hello'], "hello.txt");
    //fd.append('f1', file);
    //var expected_keys = ['n1', 'n3', 'n1', 'n3', 'f1'];
    //var expected_values = ['v1', 'v3', 'v4', 'v6', file];

    {
      var mykeys = [],
        myvalues = [];
      for (var entry of fd) {
        strictEqual(entry.length, 2);
        mykeys.push(entry[0]);
        myvalues.push(entry[1]);
      }
      deepStrictEqual(mykeys, expected_keys);
      deepStrictEqual(myvalues, expected_values);
    }

    {
      var mykeys = [],
        myvalues = [];
      for (var entry of fd.entries()) {
        strictEqual(
          entry.length,
          2,
          'entries() iterator should yield key/value pairs'
        );
        mykeys.push(entry[0]);
        myvalues.push(entry[1]);
      }
      deepStrictEqual(
        mykeys,
        expected_keys,
        'entries() iterator should see duplicate keys'
      );
      deepStrictEqual(
        myvalues,
        expected_values,
        'entries() iterator should see non-deleted values'
      );
    }

    {
      var mykeys = [];
      for (var entry of fd.keys()) mykeys.push(entry);
      deepStrictEqual(mykeys, expected_keys);
    }

    {
      var myvalues = [];
      for (var entry of fd.values()) myvalues.push(entry);
      deepStrictEqual(
        myvalues,
        expected_values,
        'values() iterator should see non-deleted values'
      );
    }
  },
};

export const w3cTestFormDataGet = {
  test() {
    strictEqual(
      create_formdata(['key', 'value1'], ['key', 'value2']).get('key'),
      'value1'
    );
    strictEqual(
      create_formdata(['key', 'value1'], ['key', 'value2']).get('nil'),
      null
    );
    strictEqual(create_formdata().get('key'), null);
    deepStrictEqual(
      create_formdata(['key', 'value1'], ['key', 'value2']).getAll('key'),
      ['value1', 'value2']
    );
    deepStrictEqual(
      create_formdata(['key', 'value1'], ['key', 'value2']).getAll('nil'),
      []
    );
    deepStrictEqual(create_formdata().getAll('key'), []);

    function create_formdata() {
      var fd = new FormData();
      for (var i = 0; i < arguments.length; i++) {
        fd.append.apply(fd, arguments[i]);
      }
      return fd;
    }
  },
};

export const w3cTestFormDataHas = {
  test() {
    strictEqual(
      create_formdata(['key', 'value1'], ['key', 'value2']).has('key'),
      true
    );
    strictEqual(
      create_formdata(['key', 'value1'], ['key', 'value2']).has('nil'),
      false
    );
    strictEqual(create_formdata().has('key'), false);

    function create_formdata() {
      var fd = new FormData();
      for (var i = 0; i < arguments.length; i++) {
        fd.append.apply(fd, arguments[i]);
      }
      return fd;
    }
  },
};

export const w3cTestFormDataSet = {
  test() {
    function test_formdata(creator, verifier, description) {
      let result = [];
      for (let [k, v] of creator()) {
        result.push(`${k}=${v}`);
      }
      verifier(result.join(','));
    }

    test_formdata(
      function () {
        var fd = new FormData();
        fd.set('name', new String('value'));
        return fd;
      },
      function (data) {
        strictEqual(data, 'name=value');
      },
      'Passing a String object to FormData.set should work'
    );

    strictEqual(create_formdata(['key', 'value1']).get('key'), 'value1');
    strictEqual(
      create_formdata(['key', 'value2'], ['key', 'value1']).get('key'),
      'value1'
    );
    strictEqual(create_formdata(['key', undefined]).get('key'), 'undefined');
    strictEqual(
      create_formdata(['key', undefined], ['key', 'value1']).get('key'),
      'value1'
    );
    strictEqual(create_formdata(['key', null]).get('key'), 'null');
    strictEqual(
      create_formdata(['key', null], ['key', 'value1']).get('key'),
      'value1'
    );

    // TODO(conform): Support File/Blob.
    //test(function () {
    //  var fd = new FormData();
    //  fd.set('key', new Blob([]), 'blank.txt');
    //  var file = fd.get('key');
    //  assert_true(file instanceof File);
    //  assert_equals(file.name, 'blank.txt');
    //}, 'testFormDataSetEmptyBlob');

    function create_formdata() {
      var fd = new FormData();
      for (var i = 0; i < arguments.length; i++) {
        fd.set.apply(fd, arguments[i]);
      }
      return fd;
    }
  },
};

export const w3cTestFormData = {
  test() {
    function do_test(name, fd, expected) {
      let result = [];
      for (let [k, v] of fd) {
        result.push(`${k}=${v}`);
      }
      strictEqual(result.join(','), expected, name);
    }

    function create_formdata() {
      var fd = new FormData();
      for (var i = 0; i < arguments.length; i++) {
        fd.append.apply(fd, arguments[i]);
      }
      return fd;
    }

    do_test('empty formdata', new FormData(), '');
    do_test(
      'formdata with string',
      create_formdata(['key', 'value']),
      'key=value'
    );
    //do_test("formdata with named string", create_formdata(['key', new Blob(['value'], {type: 'text/plain'}), 'kv.txt']), '\nkey=kv.txt:text/plain:5,');
  },
};
