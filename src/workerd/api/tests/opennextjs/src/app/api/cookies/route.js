// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { NextResponse } from 'next/server';
import { cookies } from 'next/headers';

export async function GET() {
  const cookieStore = await cookies();
  const allCookies = {};

  cookieStore.getAll().forEach((cookie) => {
    allCookies[cookie.name] = cookie.value;
  });

  return NextResponse.json({
    message: 'Cookies API',
    timestamp: Date.now(),
    cookies: allCookies,
  });
}

export async function POST(request) {
  const cookieStore = await cookies();
  const body = await request.json().catch(() => ({}));
  const { name, value, options } = body;

  if (name && value) {
    cookieStore.set(name, value, options || {});
  }

  return NextResponse.json({
    message: 'Cookie set',
    timestamp: Date.now(),
    cookie: { name, value },
  });
}

export async function DELETE(request) {
  const cookieStore = await cookies();
  const name = request.nextUrl.searchParams.get('name');

  if (name) {
    cookieStore.delete(name);
  }

  return NextResponse.json({
    message: 'Cookie deleted',
    timestamp: Date.now(),
    deleted: name,
  });
}
