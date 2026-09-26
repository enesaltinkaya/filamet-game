#!/usr/bin/env python3
"""Extract a GLB's embedded raster textures as external KTX2 files.

The packed models (test2/eve) ship their textures as PNG bufferViews inside
the GLB; the runtime decodes them with libpng single-threaded at world load
(~150 ms props + ~190 ms eve — plans/world-load.md phase 2). This script
rewrites the staged GLB so every image becomes an external
`../images/models/<model>/<name>_<semantic>.ktx2` URI (resolved against the
loader's "models/" base dir to the pak path images/models/<model>/...),
with the pixel payload converted by toktx to raw RGBA8 + KTX2 zstd
supercompression (the terrain ktx2 recipe — no Basis/BC, a pure container
change with zero quality risk; the engine's ktx2 decoder handles
KTX_SS_ZSTD).

Single mip level on purpose: the embedded-PNG path the A/B runs against has
no mipmaps either, so adding them here would move the diff above the TAA
noise floor. BC7/UASTC + --genmipmap is a separate, visual follow-up.

The suffix semantic also carries the transfer function for the loader:
_normalTexture/_occlusionTexture/_metallicRoughnessTexture/... are linear,
color slots are srgb (GltfDiligent.cpp keys the Diligent format off the
file name, one concrete format per file).

Usage: gltf-extract-textures.py <model-name> <images-out-dir> <in.glb> <out.glb>
"""
import io
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

TOKTX = Path("/home/enes/Sdks/ktx-4.4.2/bin/toktx")
KTX_LD_LIBRARY_PATH = "/home/enes/Sdks/ktx-4.4.2/lib/"

SEMANTICS = [
    ("baseColorTexture", "srgb"),
    ("metallicRoughnessTexture", "linear"),
    ("normalTexture", "linear"),
    ("occlusionTexture", "linear"),
    ("emissiveTexture", "srgb"),
    ("specularTexture", "linear"),
    ("diffuseTexture", "srgb"),
    ("specularGlossinessTexture", "linear"),
    ("clearcoatTexture", "srgb"),
    ("clearcoatRoughnessTexture", "linear"),
    ("clearcoatNormalTexture", "linear"),
    ("sheenColorTexture", "srgb"),
    ("sheenRoughnessTexture", "linear"),
]

SLOT_PATHS = [
    ("pbrMetallicRoughness", "baseColorTexture"),
    ("pbrMetallicRoughness", "metallicRoughnessTexture"),
    (None, "normalTexture"),
    (None, "occlusionTexture"),
    (None, "emissiveTexture"),
    ("extensions.KHR_materials_specular", "specularTexture"),
    ("extensions.KHR_materials_pbrSpecularGlossiness", "diffuseTexture"),
    ("extensions.KHR_materials_pbrSpecularGlossiness", "specularGlossinessTexture"),
    ("extensions.KHR_materials_clearcoat", "clearcoatTexture"),
    ("extensions.KHR_materials_clearcoat", "clearcoatRoughnessTexture"),
    ("extensions.KHR_materials_clearcoat", "clearcoatNormalTexture"),
    ("extensions.KHR_materials_sheen", "sheenColorTexture"),
    ("extensions.KHR_materials_sheen", "sheenRoughnessTexture"),
]


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


def imageUsages(gltf):
    usages = {}
    for mat in gltf.get("materials", []):
        for parent, slot in SLOT_PATHS:
            node = mat
            if parent is not None:
                for part in parent.split("."):
                    node = node.get(part) if isinstance(node, dict) else None
                    if node is None:
                        break
            if not isinstance(node, dict):
                continue
            ref = node.get(slot)
            if isinstance(ref, dict) and "index" in ref:
                usages.setdefault(ref["index"], set()).add(slot)
    return usages


def semanticFor(idx, usages):
    used = usages.get(idx, set())
    oetf_by_name = dict(SEMANTICS)
    for name, _ in SEMANTICS:
        if name in used:
            all_linear = all(oetf_by_name.get(n, "linear") == "linear" for n in used)
            return name, ("linear" if all_linear else "srgb")
    return "texture", "linear"


def sanitize(name):
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_.")
    return name or "texture"


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: gltf-extract-textures.py <model-name> <images-out-dir> <in.glb> <out.glb>")
    model, out_dir, in_path, out_path = sys.argv[1:5]
    gltf, b = read_glb(in_path)
    images = gltf.get("images", [])
    if not any("bufferView" in img for img in images):
        write_glb(gltf, b, out_path)
        print("textures: none embedded")
        return

    usages = imageUsages(gltf)
    views = gltf.get("bufferViews", [])
    accessors = gltf.get("accessors", [])
    image_views = {img["bufferView"] for img in images if "bufferView" in img}
    acc_views = {a["bufferView"] for a in accessors if "bufferView" in a}
    overlap = image_views & acc_views
    if overlap:
        raise SystemExit(f"image bufferViews also used by accessors: {sorted(overlap)}")

    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, "LD_LIBRARY_PATH": KTX_LD_LIBRARY_PATH}

    used_names = set()
    extracted = 0
    for idx, img in enumerate(images):
        if "bufferView" not in img:
            continue
        bv = views[img["bufferView"]]
        start = bv.get("byteOffset", 0)
        raw = bytes(b[start:start + bv["byteLength"]])
        semantic, oetf = semanticFor(idx, usages)
        base = sanitize(img.get("name") or f"image{idx}") + "_" + semantic
        name = base
        n = 1
        while name in used_names:
            n += 1
            name = f"{base}_{n}"
        used_names.add(name)

        normalized = Image.open(io.BytesIO(raw)).convert("RGBA")
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
            normalized.save(tmp, format="PNG")
            tmp_png = tmp.name
        ktx_path = out / f"{name}.ktx2"
        try:
            subprocess.run(
                [str(TOKTX), "--2d", "--assign_oetf", oetf,
                 "--assign_primaries", "bt709", "--zcmp", "19",
                 str(ktx_path), tmp_png],
                check=True, env=env, capture_output=True)
        finally:
            os.unlink(tmp_png)

        img.pop("bufferView")
        img.pop("mimeType", None)
        img["uri"] = f"../images/models/{model}/{name}.ktx2"
        extracted += 1
        print(f"textures: {name}.ktx2 ({normalized.width}x{normalized.height}, {oetf})")

    new_views = []
    remap = {}
    for i, bv in enumerate(views):
        if i in image_views:
            continue
        remap[i] = len(new_views)
        new_views.append(bv)
    gltf["bufferViews"] = new_views

    compact = bytearray()
    for bv in new_views:
        start = bv.get("byteOffset", 0)
        length = bv["byteLength"]
        while len(compact) % 4:
            compact.append(0)
        bv["byteOffset"] = len(compact)
        compact += b[start:start + length]

    for a in accessors:
        if "bufferView" in a:
            a["bufferView"] = remap[a["bufferView"]]

    write_glb(gltf, compact, out_path)
    print(f"textures: extracted {extracted} -> {out} (glb bin {len(b)} -> {len(compact)} bytes)")


if __name__ == "__main__":
    main()
