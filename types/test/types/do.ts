import { DurableObject } from "cloudflare:workers";
import { expectTypeOf } from "expect-type";

// Aliased as SqlStorageValue, but let's assert it includes the raw types we expect
type Value = ArrayBuffer | string | number | null;

class TestDOSql extends DurableObject {
  test() {
    const db = this.ctx.storage.sql;

    expectTypeOf<SqlStorage>(db);

    expectTypeOf<number>(db.databaseSize);

    // Verify default row type of exec
    for (const row of db.exec("...")) {
      expectTypeOf<Record<string, Value>>(row);
    }

    // Verify scoped row type of exec
    for (const row of db.exec<{ name: string; phone: number }>("...")) {
      expectTypeOf<{ name: string; phone: number }>(row);

      // @ts-expect-error double-checking our assertions are strict
      expectTypeOf<{ name: string; phone: string }>(row);
      // @ts-expect-error double-checking our assertions are strict
      expectTypeOf<Record<string, number>>(row);
    }

    const cursor = db.exec("...", 1, "two")
    expectTypeOf<SqlStorageCursor<Record<string, Value>>>(cursor);

    expectTypeOf<number>(cursor.rowsRead);
    expectTypeOf<number>(cursor.rowsWritten);
    expectTypeOf<string[]>(cursor.columnNames);

    expectTypeOf<Record<string, Value>[]>(cursor.toArray());
    expectTypeOf<Record<string, Value>>(cursor.one());

    const next = cursor.next();
    // Can narrow the type by checking .done
    if (!next.done) {
      expectTypeOf<Record<string, Value>>(next.value);
    }

    const another = cursor.next()
    // Or check .value to do the same thing
    if (another.value) {
      expectTypeOf<Record<string, Value>>(another.value);
      expectTypeOf<false | undefined>(another.done);
    } else {
      expectTypeOf<undefined>(another.value);
      expectTypeOf<true>(another.done);
    }

    // Common shorthand usage
    const { value: thirdRow } = cursor.next()
    if (!thirdRow) throw new Error('No value!')
    expectTypeOf<Record<string, Value>>(thirdRow);
  }
}

class TestDOSyncKv extends DurableObject {
  test() {
    const kv = this.ctx.storage.kv;

    // The default projection yields key/value entries.
    for (const entry of kv.list()) {
      expectTypeOf<[string, unknown]>(entry);
    }

    // Stating "entries" explicitly is the same as the default: it needs no overload of its own,
    // because the default signature already accepts it.
    for (const entry of kv.list({ projection: "entries" })) {
      expectTypeOf<[string, unknown]>(entry);
    }

    // Including when a value type is supplied alongside it.
    for (const [key, value] of kv.list<number>({ projection: "entries" })) {
      expectTypeOf<string>(key);
      expectTypeOf<number>(value);
    }

    // The value type parameter applies to the value half of each entry.
    for (const [key, value] of kv.list<number>()) {
      expectTypeOf<string>(key);
      expectTypeOf<number>(value);
    }

    // "keys" narrows the element type to the key alone.
    for (const key of kv.list({ projection: "keys" })) {
      expectTypeOf<string>(key);

      // @ts-expect-error double-checking our assertions are strict
      expectTypeOf<[string, unknown]>(key);
    }

    // "values" narrows the element type to the value alone, and stays generic.
    for (const value of kv.list<number>({ projection: "values" })) {
      expectTypeOf<number>(value);

      // @ts-expect-error double-checking our assertions are strict
      expectTypeOf<string>(value);
    }

    // Projections combine with the other list options.
    for (const key of kv.list({
      projection: "keys",
      prefix: "a",
      reverse: true,
      limit: 1,
    })) {
      expectTypeOf<string>(key);
    }

    // @ts-expect-error an unrecognized projection is rejected at compile time
    kv.list({ projection: "nope" });

    // A projection held in a variable still narrows to its literal type, so it picks the same
    // overload as writing the literal inline.
    const fromVariable = "keys";
    expectTypeOf<Iterable<string>>(kv.list({ projection: fromVariable }));

    // Annotating the variable with a wider union does not change that: a const narrows to its
    // initializer.
    const fromAnnotated: "keys" | "entries" = "keys";
    expectTypeOf<Iterable<string>>(kv.list({ projection: fromAnnotated }));

    // A projection whose type has widened all the way to `string` matches no overload at all, so
    // it is rejected rather than silently mistyped.
    let mutable = "keys";
    // @ts-expect-error
    kv.list({ projection: mutable });
  }

  // A projection that is still a union where list() is called cannot select the "keys" or
  // "values" overload, so it falls back to the entries signature and the element type is reported
  // as a pair even for the runs that yield bare keys. The values are correct at runtime — only the
  // static type is wrong. Narrow the projection before calling, or call once per branch.
  listWithDynamicProjection(projection: "keys" | "entries") {
    const iterator = this.ctx.storage.kv.list({ projection });
    expectTypeOf<Iterable<[string, unknown]>>(iterator);
  }

  // The other ways a projection can reach list() without its literal type intact.
  listWithPreBuiltOptions(cond: boolean) {
    const kv = this.ctx.storage.kv;

    // Annotating the options object with the options type erases the literal, so this hits the
    // same fall-through as a union parameter: typed as pairs, yields bare keys. Prefer `as const`,
    // or pass the object inline.
    const annotated: SyncKvListOptions = { projection: "keys", limit: 10 };
    expectTypeOf<Iterable<[string, unknown]>>(kv.list(annotated));

    // Without the annotation the property widens to `string`, which matches no overload and so is
    // rejected outright rather than mistyped. Note that the expression still has a type for the
    // sake of error recovery — TypeScript keeps the best-match overload's `Iterable<string>` —
    // so a tool that only inspects the type and ignores diagnostics will see nothing wrong here.
    const inferred = { projection: "keys", limit: 10 };
    // @ts-expect-error
    kv.list(inferred);

    // `as const` keeps the literal and selects the keys overload.
    const asConst = { projection: "keys", limit: 10 } as const;
    expectTypeOf<Iterable<string>>(kv.list(asConst));

    // A projection computed inline is a union at the call site, so it falls through too.
    expectTypeOf<Iterable<[string, unknown]>>(
      kv.list({ projection: cond ? "keys" : "values" })
    );

    // An explicit `undefined` means "not supplied", which is the default, so the reported element
    // type and the runtime shape agree.
    expectTypeOf<Iterable<[string, unknown]>>(
      kv.list({ projection: undefined })
    );

    // The keys overload declares an unused type parameter purely so that supplying one does not
    // skip it in favour of the entries signature. There is no value type to name when projecting
    // keys, so the argument is accepted and ignored rather than silently changing the element
    // type to a pair.
    expectTypeOf<Iterable<string>>(kv.list<number>({ projection: "keys" }));
  }
}
