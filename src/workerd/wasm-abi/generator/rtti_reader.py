# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Reads a JSG RTTI dump (an unpacked Cap'n Proto `StructureGroups` message, see
`src/workerd/jsg/rtti.capnp`) into plain Python dictionaries.

The reader is self-contained so the generator runs with a bare `python3`. It implements the subset
of the Cap'n Proto encoding that the RTTI uses: segment tables, struct and list pointers, far
pointers, composite lists, text and booleans. The field offsets below are the layout that
`capnp compile -ocapnp src/workerd/jsg/rtti.capnp` reports. If they drift from the schema, the
self-test fails, because the modules no longer come out as checked in.
"""

import struct


class Message:
    def __init__(self, data):
        (count_minus_one,) = struct.unpack_from("<I", data, 0)
        count = count_minus_one + 1
        sizes = struct.unpack_from(f"<{count}I", data, 4)
        offset = 4 + 4 * count
        if offset % 8:
            offset += 4
        self.segments = []
        for size in sizes:
            self.segments.append(memoryview(data)[offset : offset + size * 8])
            offset += size * 8

    def word(self, seg, index):
        return struct.unpack_from("<Q", self.segments[seg], index * 8)[0]

    def resolve(self, seg, index):
        """Returns (seg, pointer_index, pointer_word, target_index) for the pointer at
        (seg, index), following far pointers. Returns None for a null pointer."""
        ptr = self.word(seg, index)
        if ptr == 0:
            return None
        kind = ptr & 3
        if kind == 2:
            double = (ptr >> 2) & 1
            pad_offset = (ptr >> 3) & 0x1FFFFFFF
            pad_seg = ptr >> 32
            if not double:
                pad = self.word(pad_seg, pad_offset)
                offset = _signed30(pad >> 2)
                return (pad_seg, pad_offset, pad, pad_offset + 1 + offset)
            landing = self.word(pad_seg, pad_offset)
            tag = self.word(pad_seg, pad_offset + 1)
            target_seg = landing >> 32
            target_index = (landing >> 3) & 0x1FFFFFFF
            return (target_seg, None, tag, target_index)
        offset = _signed30(ptr >> 2)
        return (seg, index, ptr, index + 1 + offset)

    def root(self):
        return self.struct_at(0, 0)

    def struct_at(self, seg, index):
        r = self.resolve(seg, index)
        if r is None:
            return None
        target_seg, _, ptr, target = r
        assert ptr & 3 == 0, "expected struct pointer"
        data_words = (ptr >> 32) & 0xFFFF
        ptr_words = ptr >> 48
        return Struct(self, target_seg, target, data_words, ptr_words)

    def list_at(self, seg, index):
        r = self.resolve(seg, index)
        if r is None:
            return None
        target_seg, _, ptr, target = r
        assert ptr & 3 == 1, "expected list pointer"
        elem_size = (ptr >> 32) & 7
        count = ptr >> 35
        return (target_seg, target, elem_size, count)


def _signed30(v):
    v &= 0x3FFFFFFF
    return v - (1 << 30) if v & (1 << 29) else v


class Struct:
    def __init__(self, msg, seg, index, data_words, ptr_words):
        self.msg = msg
        self.seg = seg
        self.index = index
        self.data_words = data_words
        self.ptr_words = ptr_words

    def u16(self, bit_offset):
        byte = bit_offset // 8
        if byte + 2 > self.data_words * 8:
            return 0
        return struct.unpack_from(
            "<H", self.msg.segments[self.seg], self.index * 8 + byte
        )[0]

    def bool(self, bit):
        byte = bit // 8
        if byte >= self.data_words * 8:
            return False
        return bool(
            self.msg.segments[self.seg][self.index * 8 + byte] & (1 << (bit % 8))
        )

    def i64(self, bit_offset):
        byte = bit_offset // 8
        if byte + 8 > self.data_words * 8:
            return 0
        return struct.unpack_from(
            "<q", self.msg.segments[self.seg], self.index * 8 + byte
        )[0]

    def f64(self, bit_offset):
        byte = bit_offset // 8
        if byte + 8 > self.data_words * 8:
            return 0.0
        return struct.unpack_from(
            "<d", self.msg.segments[self.seg], self.index * 8 + byte
        )[0]

    def _ptr_index(self, n):
        if n >= self.ptr_words:
            return None
        return self.index + self.data_words + n

    def struct(self, n):
        i = self._ptr_index(n)
        return None if i is None else self.msg.struct_at(self.seg, i)

    def text(self, n):
        i = self._ptr_index(n)
        if i is None:
            return ""
        lst = self.msg.list_at(self.seg, i)
        if lst is None:
            return ""
        seg, target, elem_size, count = lst
        assert elem_size == 2, "text must be a byte list"
        raw = bytes(self.msg.segments[seg][target * 8 : target * 8 + count])
        return raw.rstrip(b"\0").decode("utf-8")

    def struct_list(self, n):
        i = self._ptr_index(n)
        if i is None:
            return []
        lst = self.msg.list_at(self.seg, i)
        if lst is None:
            return []
        seg, target, elem_size, count = lst
        assert elem_size == 7, "expected composite list"
        tag = self.msg.word(seg, target)
        elems = (tag >> 2) & 0x3FFFFFFF
        data_words = (tag >> 32) & 0xFFFF
        ptr_words = tag >> 48
        step = data_words + ptr_words
        return [
            Struct(self.msg, seg, target + 1 + k * step, data_words, ptr_words)
            for k in range(elems)
        ]

    def text_list(self, n):
        i = self._ptr_index(n)
        if i is None:
            return []
        lst = self.msg.list_at(self.seg, i)
        if lst is None:
            return []
        seg, target, elem_size, count = lst
        assert elem_size == 6, "expected pointer list"
        out = []
        for k in range(count):
            fake = Struct(self.msg, seg, target + k, 0, 1)
            out.append(fake.text(0))
        return out


# ---- Schema-specific decoding (layout from `capnp compile -ocapnp`). ----

TYPE_KINDS = [
    "unknown",
    "voidt",
    "boolt",
    "number",
    "promise",
    "structure",
    "string",
    "object",
    "array",
    "maybe",
    "dict",
    "oneOf",
    "builtin",
    "intrinsic",
    "function",
    "jsgImpl",
    "jsBuiltin",
]

BUILTIN_TYPES = [
    "v8Uint8Array",
    "v8ArrayBufferView",
    "jsgBufferSource",
    "kjDate",
    "v8Function",
    "v8ArrayBuffer",
    "v8Uint16Array",
    "v8Uint32Array",
    "v8Int8Array",
    "v8Int16Array",
    "v8Int32Array",
    "v8Float32Array",
    "v8Float64Array",
    "v8BigInt64Array",
    "v8BigUint64Array",
    "v8Uint8ClampedArray",
    "v8Symbol",
    "v8TypedArray",
    "jsgError",
]

JSG_IMPL_TYPES = [
    "configuration",
    "v8Isolate",
    "jsgLock",
    "jsgTypeHandler",
    "jsgUnimplemented",
    "jsgVarargs",
    "jsgSelfRef",
    "v8FunctionCallbackInfo",
    "v8PropertyCallbackInfo",
    "jsgName",
]


def decode_type(s):
    """Type: 8 bytes, 1 ptr; union tag at bits [0, 16), payload at ptr[0]."""
    if s is None:
        return {"kind": "unknown"}
    kind = TYPE_KINDS[s.u16(0)]
    p = s.struct(0)
    if kind in ("unknown", "voidt", "boolt", "object"):
        return {"kind": kind}
    if kind in ("number", "string", "intrinsic"):
        return {"kind": kind, "name": p.text(0) if p else ""}
    if kind == "promise":
        return {"kind": kind, "value": decode_type(p.struct(0))}
    if kind == "structure":
        return {"kind": kind, "name": p.text(0), "fullyQualifiedName": p.text(1)}
    if kind == "array":
        return {"kind": kind, "element": decode_type(p.struct(0)), "name": p.text(1)}
    if kind == "maybe":
        return {
            "kind": kind,
            "value": decode_type(p.struct(0)),
            "name": p.text(1),
            "allowsNull": p.bool(0),
            "allowsUndefined": p.bool(1),
        }
    if kind == "dict":
        return {
            "kind": kind,
            "key": decode_type(p.struct(0)),
            "value": decode_type(p.struct(1)),
        }
    if kind == "oneOf":
        return {"kind": kind, "variants": [decode_type(v) for v in p.struct_list(0)]}
    if kind == "builtin":
        return {"kind": kind, "type": BUILTIN_TYPES[p.u16(0)]}
    if kind == "function":
        return decode_function(p)
    if kind == "jsgImpl":
        return {"kind": kind, "type": JSG_IMPL_TYPES[p.u16(0)]}
    if kind == "jsBuiltin":
        return {"kind": kind, "module": p.text(0), "export": p.text(1)}
    raise AssertionError(kind)


def decode_function(p):
    """FunctionType: 0 bytes, 2 ptrs (returnType, args)."""
    return {
        "kind": "function",
        "returnType": decode_type(p.struct(0)),
        "args": [decode_type(a) for a in p.struct_list(1)],
    }


def decode_method(p):
    """Method: 8 bytes, 4 ptrs (name, returnType, args, argNames); static at bit 0."""
    if p is None:
        return None
    return {
        "name": p.text(0),
        "returnType": decode_type(p.struct(1)),
        "args": [decode_type(a) for a in p.struct_list(2)],
        "static": p.bool(0),
        "argNames": p.text_list(3),
    }


def decode_member(m):
    """Member: 8 bytes, 2 ptrs; union tag at bits [0, 16)."""
    tag = m.u16(0)
    p = m.struct(0)
    if tag == 0:
        return {"method": decode_method(p)}
    if tag == 1:
        return {
            "property": {
                "name": p.text(0),
                "type": decode_type(p.struct(1)),
                "readonly": p.bool(0),
                "lazy": p.bool(1),
                "prototype": p.bool(2),
            }
        }
    if tag == 2:
        return {"nested": {"structure": decode_structure(p), "name": m.text(1)}}
    if tag == 3:
        is_number = p.u16(64) == 1
        return {
            "constant": {
                "name": p.text(0),
                "value": p.f64(0) if is_number else p.i64(0),
            }
        }
    if tag == 4:
        return {
            "constructor": {
                "args": [decode_type(a) for a in p.struct_list(0)],
                "argNames": p.text_list(1),
            }
        }
    raise AssertionError(tag)


def decode_structure(s):
    """Structure: 8 bytes, 13 ptrs."""
    callable_ptr = s.struct(8)
    return {
        "name": s.text(0),
        "fullyQualifiedName": s.text(3),
        "members": [decode_member(m) for m in s.struct_list(1)],
        "extends": decode_type(s.struct(2)) if s.struct(2) is not None else None,
        "iterable": s.bool(0),
        "iterator": decode_method(s.struct(4)),
        "asyncIterable": s.bool(1),
        "asyncIterator": decode_method(s.struct(5)),
        "callable": decode_function(callable_ptr) if callable_ptr is not None else None,
        "tsName": s.text(12),
    }


def read_rtti(path):
    with open(path, "rb") as f:
        data = f.read()
    root = Message(data).root()
    groups = [
        {
            "name": g.text(0),
            "structures": [decode_structure(s) for s in g.struct_list(1)],
        }
        for g in root.struct_list(0)
    ]
    return {"groups": groups}


def structures_by_name(rtti):
    """Maps fully-qualified names (and plain names, when unambiguous) to structures."""
    out = {}
    for g in rtti["groups"]:
        for s in g["structures"]:
            out[s["fullyQualifiedName"]] = s
    return out
