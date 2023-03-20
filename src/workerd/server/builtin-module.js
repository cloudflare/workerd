import secret from "test-internal:builtin-internal-module";

export function openDoor(key) {
  if (key != secret.caveKey) throw new Error("Wrong key: " + key);
  return true;
}
