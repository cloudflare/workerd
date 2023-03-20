import secrets from "alibaba-internal:secrets";

export function open(key) {
  return key === secrets.caveKey;
}
