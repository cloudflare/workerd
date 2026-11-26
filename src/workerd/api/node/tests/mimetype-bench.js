// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// node:util MIMEType and MIMEParams: parsing, serializing and the parameter operations. The
// default variant parses with the C++ MimeType and @all-autogates with the Rust one.

import { MIMEType } from 'node:util';

const params = (n) =>
  Array.from({ length: n }, (_, i) => `p${i}=v${i}`).join(';');
const TWO = `text/plain;${params(2)}`;
const TWENTY = `text/plain;${params(20)}`;
const QUOTED = 'text/plain;a="x y";b="q\\"r";charset=utf-8';

export default {
  bench(b) {
    b.run('parse text/plain', () => b.blackBox(new MIMEType('text/plain')));
    b.run('parse with 2 params', () => b.blackBox(new MIMEType(TWO)));
    b.run('parse with 20 params', () => b.blackBox(new MIMEType(TWENTY)));
    b.run('parse quoted values', () => b.blackBox(new MIMEType(QUOTED)));
    b.run('parse invalid (throws)', () => {
      try {
        new MIMEType('bogus');
      } catch (e) {
        b.blackBox(e);
      }
    });

    const two = new MIMEType(TWO);
    const twenty = new MIMEType(TWENTY);
    b.run('toString(), 2 params', () => b.blackBox(two.toString()));
    b.run('toString(), 20 params', () => b.blackBox(twenty.toString()));
    b.run('params.toString(), 20 params', () =>
      b.blackBox(twenty.params.toString())
    );
    b.run('essence', () => b.blackBox(two.essence));
    b.run('params.get()', () => b.blackBox(two.params.get('p1')));
    b.run('params.set() + delete()', () => {
      two.params.set('extra', 'value');
      two.params.delete('extra');
    });
    b.run('iterate 20 params', () => {
      for (const entry of twenty.params) b.blackBox(entry);
    });
    b.run('type setter', () => {
      two.type = 'application';
    });
  },
};
