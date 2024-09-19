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
    if (!next.done) {
      expectTypeOf<Record<string, Value>>(next.value);
    }
  }
}
