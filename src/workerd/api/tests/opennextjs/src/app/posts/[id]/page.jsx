// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default async function PostPage({ params }) {
  const { id } = await params;

  return (
    <main>
      <h1>Post {id}</h1>
      <p id="post-id">Post ID: {id}</p>
      <p id="render-time">Rendered at: {Date.now()}</p>
    </main>
  );
}
