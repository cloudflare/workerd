// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { redirect } from 'next/navigation';

export default function RedirectTestPage({ searchParams }) {
  return <RedirectHandler searchParams={searchParams} />;
}

async function RedirectHandler({ searchParams }) {
  const params = await searchParams;
  if (params.target) {
    redirect(params.target);
  }
  return (
    <main>
      <h1>Redirect Test</h1>
      <p>Add ?target=/path to redirect</p>
    </main>
  );
}
