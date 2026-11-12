// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/* global CloneableTestBox */

import { WorkerEntrypoint } from 'cloudflare:workers';

export default class extends WorkerEntrypoint {
  describe(box) {
    return {
      isBox: box instanceof CloneableTestBox,
      label: box.label,
      value: box.value,
    };
  }

  makeBox(label, value) {
    return new CloneableTestBox(label, value);
  }

  echo(box) {
    return box;
  }

  throwBox(label) {
    throw new CloneableTestBox(label, { thrown: true });
  }

  // Uses the function, stub and stream in the box's value.
  async useValue(box) {
    const { fn, target, stream } = box.value;
    return [
      await fn(),
      await target.hello(),
      await new Response(stream).text(),
    ];
  }
}
