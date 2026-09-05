#!/usr/bin/env python3
"""Convert gltfpack's quantized rotation keyframes to fp32.

gltfpack always writes animation rotation sampler outputs as 16-bit
NORMALIZED snorm quaternions (hard-coded in writeKeyframeStream — `-noq` and
`-ar` do not change that). Diligent's GLTF loader only reads fp32 sampler
outputs: its int16 path is a compiled-out VERIFY, so the raw shorts are
reinterpreted as floats and the skeleton NaNs out.

This script rewrites every rotation output accessor (componentType SHORT,
normalized) as fp32: value = max(v / 32767, -1). It must run AFTER gltfpack
(scripts/export-models.sh), before the zstd pass.

Usage: gltf-rotation-f32.py input.glb output.glb
"""
import json
import struct
import sys

FLOAT = 5126
SHORT = 5122
TYPES = {"SCALAR": 1, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, total = struct.unpack_from("<4sII", data, 0)
    assert magic == b"glTF"
    gltf = None
    bin_data = b""
    off = 12
    while off < total:
        clen, ctype = struct.unpack_from("<I4s", data, off)
        chunk = data[off + 8:off + 8 + clen]
        if ctype == b"JSON":
            gltf = json.loads(chunk)
        elif ctype == b"BIN\x00":
            bin_data = chunk
        off += 8 + clen
    return gltf, bytearray(bin_data)


def write_glb(gltf, bin_data, path):
    # the BIN chunk may have grown (appended fp32 accessors) — the declared
    # buffer length must cover it (lessons 2026-09-04: "update buffers[].byteLength")
    if bin_data and gltf.get("buffers"):
        gltf["buffers"][0]["byteLength"] = len(bin_data)
    json_data = json.dumps(gltf, separators=(",", ":")).encode()
    pad = (4 - len(json_data) % 4) % 4
    json_data += b" " * pad
    body = struct.pack("<I4s", len(json_data), b"JSON") + json_data
    if bin_data:
        while len(bin_data) % 4:
            bin_data.append(0)
        body += struct.pack("<I4s", len(bin_data), b"BIN\x00") + bytes(bin_data)
    total = 12 + len(body)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total) + body)


def snorm16_to_f32(gltf, b, acc_index):
    """Rewrite one normalized-int16 accessor as a new fp32 accessor."""
    acc = gltf["accessors"][acc_index]
    ncomp = TYPES[acc["type"]]
    bv = gltf["bufferViews"][acc["bufferView"]]
    start = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    count = acc.get("count", 0)

    raw = struct.unpack_from(f"<{ncomp * count}h", b, start)
    vals = [max(v / 32767.0, -1.0) for v in raw]

    while len(b) % 4:
        b.append(0)
    off = len(b)
    b.extend(struct.pack(f"<{ncomp * count}f", *vals))

    src_bv = gltf["bufferViews"][acc["bufferView"]]
    bv_idx = len(gltf["bufferViews"])
    gltf["bufferViews"].append({
        "buffer": src_bv.get("buffer", 0),
        "byteOffset": off,
        "byteLength": ncomp * 4 * count,
    })
    new_acc = {
        "bufferView": bv_idx,
        "byteOffset": 0,
        "componentType": FLOAT,
        "count": count,
        "type": acc["type"],
    }
    mn = [min(vals[i * ncomp + c] for i in range(count)) for c in range(ncomp)]
    mx = [max(vals[i * ncomp + c] for i in range(count)) for c in range(ncomp)]
    if count > 0:
        new_acc["min"] = mn
        new_acc["max"] = mx
    gltf["accessors"].append(new_acc)
    return len(gltf["accessors"]) - 1


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: gltf-rotation-f32.py input.glb output.glb")
    gltf, b = read_glb(sys.argv[1])

    converted = 0
    for anim in gltf.get("animations", []):
        for sam in anim.get("samplers", []):
            out_idx = sam.get("output")
            acc = gltf["accessors"][out_idx]
            if acc.get("componentType") == FLOAT:
                continue
            if acc.get("componentType") != SHORT or not acc.get("normalized"):
                raise SystemExit(
                    f"unexpected rotation accessor componentType {acc.get('componentType')} "
                    f"(normalized {acc.get('normalized')}); only gltfpack's int16 "
                    "normalized output is handled")
            sam["output"] = snorm16_to_f32(gltf, b, out_idx)
            converted += 1

    write_glb(gltf, b, sys.argv[2])
    print(f"rotations converted to f32: {converted}")


if __name__ == "__main__":
    main()
