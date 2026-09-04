#!/usr/bin/env python3
"""Make constant-value animation channels exact no-ops against the static pose.

The source scene (Blender/Mixamo) bakes some constant channels with values
that do not match the node's static TRS (e.g. an idle action whose Head scale
channel is 0.01 — the cm factor — while the Head node's static scale is 1.0),
and gltfpack then collapses those constant keyframes into single-key samplers.
gltfio applies a single-key channel by writing that constant every frame, so
a mismatched constant corrupts the pose (a Head scale of 0.01 shrinks the
head to 1%), and a constant rotation that differs from the static pose makes
clips disagree about rest poses (stale arm between run and idle).

This step rewrites every channel whose keyframes are constant (single key, or
identical keys within tolerance) to the target node's static TRS component
(identity when absent), which is a strict no-op: the channel now writes
exactly what the node already holds. It also guarantees that any joint not
animated by the active clip is reverted to its static pose every frame (no
stale pose leaking between clips). Runs on the plain (pre-gltfpack) GLB.

Usage: gltf-singlekey-fix.py input.glb output.glb
"""
import json
import struct
import sys

FLOAT = 5126
TYPES = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}


def read_glb(path):
    with open(path, "rb") as f:
        raw = f.read()
    magic, version, total = struct.unpack_from("<4sII", raw, 0)
    if magic != b"glTF" or version != 2:
        raise SystemExit(f"not a GLB v2: {path}")
    off = 12
    json_data = None
    bin_data = b""
    while off < total:
        clen, ctype = struct.unpack_from("<I4s", raw, off)
        data = raw[off + 8: off + 8 + clen]
        if ctype == b"JSON":
            json_data = data
        elif ctype == b"BIN\x00":
            bin_data = data
        off += 8 + clen
    if json_data is None:
        raise SystemExit("no JSON chunk")
    return json.loads(json_data), bin_data


def write_glb(path, gltf, bin_data):
    js = json.dumps(gltf, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if len(js) % 4:
        js += b" " * (4 - len(js) % 4)
    if len(bin_data) % 4:
        bin_data += b"\x00" * (4 - len(bin_data) % 4)
    body = struct.pack("<I4s", len(js), b"JSON") + js
    if bin_data:
        body += struct.pack("<I4s", len(bin_data), b"BIN\x00") + bin_data
    total = 12 + len(body)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total) + body)


def static_value(node, path):
    if path == "translation":
        return list(node.get("translation", [0.0, 0.0, 0.0]))[:3]
    if path == "rotation":
        return list(node.get("rotation", [0.0, 0.0, 0.0, 1.0]))[:4]
    if path == "scale":
        return list(node.get("scale", [1.0, 1.0, 1.0]))[:3]
    return None


def read_accessor(gltf, bin_data, acc_index):
    acc = gltf["accessors"][acc_index]
    if acc.get("componentType") != FLOAT:
        raise SystemExit(f"accessor {acc_index} is not FLOAT; refusing to touch it")
    bv = gltf["bufferViews"][acc["bufferView"]]
    ncomp = TYPES[acc["type"]]
    count = acc.get("count", 0)
    stride = bv.get("byteStride", ncomp * 4) or ncomp * 4
    start = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    vals = []
    for i in range(count):
        vals.append(struct.unpack_from(f"<{ncomp}f", bin_data, start + i * stride))
    return vals, start, stride, ncomp


def is_constant(vals, eps=1e-5):
    if len(vals) < 2:
        return True
    ref = vals[0]
    for v in vals:
        if any(abs(a - b) > eps * max(1.0, abs(b)) for a, b in zip(v, ref)):
            return False
    return True


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: gltf-singlekey-fix.py input.glb output.glb")
    gltf, bin_data = read_glb(sys.argv[1])
    nodes = gltf["nodes"]
    b = bytearray(bin_data)

    # output accessor -> value written (shared-keyframe dedup: identical
    # constants may share storage, conflicting ones get a copy)
    acc_written = {}
    fixed = 0
    copied = 0

    def write_value(acc_index, values, ncomp):
        nonlocal b
        acc = gltf["accessors"][acc_index]
        bv = gltf["bufferViews"][acc["bufferView"]]
        stride = bv.get("byteStride", ncomp * 4) or ncomp * 4
        start = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
        for i in range(acc.get("count", 1)):
            struct.pack_into(f"<{ncomp}f", b, start + i * stride, *values)
        if "min" in acc or "max" in acc:
            acc["min"] = list(values)
            acc["max"] = list(values)

    def alloc_copy(src_acc_index, values, nkeys):
        # the copy gets nkeys identical values; gltfpack's loader requires
        # sampler output count == input count, so nkeys = the channel's key count
        nonlocal b, copied
        src = gltf["accessors"][src_acc_index]
        ncomp = TYPES[src["type"]]
        while len(b) % 4:
            b.append(0)
        off = len(b)
        b.extend(struct.pack(f"<{ncomp * nkeys}f", *([v for v in values for _ in range(nkeys)])))
        src_bv = gltf["bufferViews"][src["bufferView"]]
        bv_idx = len(gltf["bufferViews"])
        gltf["bufferViews"].append({
            "buffer": src_bv.get("buffer", 0),
            "byteOffset": off,
            "byteLength": ncomp * 4 * nkeys,
        })
        acc_idx = len(gltf["accessors"])
        gltf["accessors"].append({
            "bufferView": bv_idx,
            "byteOffset": 0,
            "componentType": src["componentType"],
            "count": nkeys,
            "type": src["type"],
            "min": list(values),
            "max": list(values),
        })
        copied += 1
        return acc_idx

    for anim in gltf.get("animations", []):
        for ch in anim["channels"]:
            tgt = ch["target"]
            node = nodes[tgt["node"]]
            if "name" not in node:
                continue
            path = tgt["path"]
            if path not in ("translation", "rotation", "scale"):
                continue
            value = static_value(node, path)
            in_acc = gltf["accessors"][anim["samplers"][ch["sampler"]]["input"]]
            out_acc = anim["samplers"][ch["sampler"]]["output"]
            in_count = in_acc.get("count", 0)
            ncomp = TYPES[gltf["accessors"][out_acc]["type"]]
            if in_count >= 2:
                # multi-key channel: only rewrite when every key is constant
                vals, start, stride, ncomp = read_accessor(gltf, b, out_acc)
                if not is_constant(vals):
                    continue
            elif in_count == 0:
                # channel has no keyframes at all — nothing to rewrite
                continue
            # write the static value to every key (keeps shared min/max
            # consistent with the data for downstream validators)
            prior = acc_written.get(out_acc)
            if prior is None:
                write_value(out_acc, value, ncomp)
                acc_written[out_acc] = value
                fixed += 1
            elif prior != value:
                new_acc = alloc_copy(out_acc, value, in_count)
                anim["samplers"][ch["sampler"]]["output"] = new_acc
                acc_written[new_acc] = value
                fixed += 1
            # else: same constant already written — nothing to do

    print(f"singlekey-fix: {fixed} constant channels set to static pose "
          f"({copied} needed a dedicated accessor)")
    while len(b) % 4:
        b.append(0)
    for buf in gltf.get("buffers", []):
        if "uri" not in buf:  # the GLB BIN buffer
            buf["byteLength"] = len(b)
    write_glb(sys.argv[2], gltf, bytes(b))
    print(f"wrote {sys.argv[2]}")


if __name__ == "__main__":
    main()
