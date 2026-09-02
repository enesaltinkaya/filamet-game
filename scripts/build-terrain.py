#!/usr/bin/env python3
"""
Terrain asset pipeline: .blend → chunked GLB + splat/style KTX2 + manifest.

Pipeline (standalone mode):
    blender → terrain-chunker (10x10) → gltfpack
    painted splat UDIM tiles → UASTC KTX2 → baked to BC7 KTX2 (ktx2bc7)
    style detail textures → merged 2K albedo/normal KTX2 (UASTC → BC7)
    manifest.json tying layers/chunks/styles together

BC7 baking (scripts/ktx2bc7.c, driven by this script) transcodes the toktx
UASTC output to raw BC7 blocks offline via libktx — the same basisu engine
the game's loader uses — so terrainInit just uploads bytes (no runtime
transcode). The game still accepts unbaked UASTC files as a fallback.

Blender mode (run by this script via `blender --python`): exports the GLB and
collects splatInfo extras (which node-group channel maps to which style
texture), mirroring the game-001-cpp exporter.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# ─── Detect which mode we are running in ───────────────────────────────────
try:
    import bpy  # noqa: F401 – available only inside Blender
    _INSIDE_BLENDER = True
except ImportError:
    _INSIDE_BLENDER = False

# ═══════════════════════════════════════════════════════════════════════════
#  Configuration
# ═══════════════════════════════════════════════════════════════════════════
BLEND_FILE     = Path("/home/enes/Projects/assets/Scenes/Terrain/oghuzlands/oghuzlands.blend")
TERRAIN_CHUNKER = Path("/home/enes/Projects/c/game-001-cpp/tools/terrain-chunker/terrain-chunker")
GLTFPACK       = Path("/home/enes/Projects/c/cpp-thirdparty/meshoptimizer/git/build-linux/gltfpack")
TOKTX          = Path("/home/enes/Sdks/ktx-4.4.2/bin/toktx")
TOKTX_LIB_PATH = "/home/enes/Sdks/ktx-4.4.2/lib"
KTX_ROOT       = TOKTX.parent.parent          # SDK include/ + lib/ for ktx2bc7
TEXTURE_ROOT   = Path("/home/enes/Projects/assets/textures")

CHUNK_GRID     = 10        # chunk grid == UDIM grid (1 chunk : 1 tile per group)
UDIM_GRID      = 10
STYLE_RES      = 2048
NOOP_MAX_SIZE  = 20_800    # solid 1024x1024 RGBA tiles are ~20.7 KB

THIS_SCRIPT    = Path(__file__).resolve()
REPO_ROOT      = THIS_SCRIPT.parent.parent
PAK_DIR        = REPO_ROOT / "c-game" / "data" / "pak_1"
OUT_MODEL_DIR  = PAK_DIR / "models" / "terrain"
OUT_TEX_DIR    = PAK_DIR / "textures" / "terrain" / "oghuzlands"
TMP_DIR        = THIS_SCRIPT.parent / ".tmp"
BAKER_SRC      = THIS_SCRIPT.parent / "ktx2bc7.c"
BAKER          = TMP_DIR / "ktx2bc7"

# glTF uv.Y vs Blender image V: resolved once by probeUvFlip() — if the
# exporter leaves V unchanged, tile PNGs are flipped vertically at build time
# so GPU top-row sampling matches Blender's painting (see probeUvFlip).


def probeUvOrientation() -> dict:
    """Determine (once, cached) how Blender's glTF exporter transforms UVs that
    span multiple UDIM tiles. Exports a 1-triangle mesh with UVs
    (0.25,0.25), (2.75,1.25), (0.5,3.75) and inspects the GLB UV accessor.

    Returns {"v": mode, "u": mode} where mode is one of
      "same"       – coordinates unchanged
      "tile"       – flipped within each tile cell (v = floor + 1 - fract)
      "one-minus"  – global flip (v = 1 - v): Blender 5.x glTF exporter behavior;
                      tile rows run 0..-9, in-tile V is GPU(top-row)-correct
      "mirror"     – whole range mirrored (v = max - v)
    """
    cacheFile = TMP_DIR / "uvorientation.txt"
    glbProbe = TMP_DIR / "uvprobe.glb"

    if cacheFile.exists():
        modes = cacheFile.read_text().strip().split(",")
        return {"v": modes[0], "u": modes[1]}

    TMP_DIR.mkdir(parents=True, exist_ok=True)
    uvs = [(0.25, 0.25), (2.75, 1.25), (0.5, 3.75)]
    expr = r'''
import bpy, bmesh
bpy.ops.wm.read_factory_settings(use_empty=True)
mesh = bpy.data.meshes.new("p")
ob = bpy.data.objects.new("p", mesh)
bpy.context.collection.objects.link(ob)
bm = bmesh.new()
v0 = bm.verts.new((0.0, 0.0, 0.0))
v1 = bm.verts.new((1.0, 0.0, 0.0))
v2 = bm.verts.new((0.0, 1.0, 0.0))
bm.faces.new((v0, v1, v2))
uvl = bm.loops.layers.uv.new("UVMap")
bm.faces.ensure_lookup_table()
uvs = %r
for loop, uv in zip(bm.faces[0].loops, uvs):
    loop[uvl].uv = uv
bm.to_mesh(mesh)
bm.free()
bpy.ops.export_scene.gltf(filepath=r"%s",
                          export_format="GLB", export_yup=True,
                          export_normals=False, export_texcoords=True,
                          export_tangents=False, export_materials="NONE")
''' % (uvs, str(glbProbe))
    run("blender", "--background", "--python-expr", expr)

    import math
    import struct

    with open(glbProbe, "rb") as f:
        data = f.read()
    jsonLen = struct.unpack_from("<I", data, 12)[0]
    gltf = json.loads(data[20:20 + jsonLen])
    prim = gltf["meshes"][0]["primitives"][0]
    uvAttr = prim["attributes"]["TEXCOORD_0"]
    accessor = gltf["accessors"][uvAttr]
    bv = gltf["bufferViews"][accessor["bufferView"]]
    binStart = 20 + jsonLen + 8 + (bv.get("byteOffset", 0)) + accessor.get("byteOffset", 0)
    count = accessor["count"]
    floats = struct.unpack_from("<%df" % (count * 2), data, binStart)
    outU = sorted(round(floats[i * 2], 4) for i in range(count))
    outV = sorted(round(floats[i * 2 + 1], 4) for i in range(count))
    glbProbe.unlink(missing_ok=True)

    def classify(source, exported, extent):
        same = sorted(source) == exported
        perTile = sorted(math.floor(v) + (1.0 - (v % 1.0)) for v in source) == exported
        oneMinus = sorted(round(1.0 - v, 4) for v in source) == exported
        mirror = sorted(round(extent - v, 4) for v in source) == exported
        if same:
            return "same"
        if perTile:
            return "tile"
        if oneMinus:
            return "one-minus"
        if mirror:
            return "mirror"
        raise RuntimeError(f"uv probe: unexpected {exported} (source {sorted(source)})")

    inU = sorted(u for u, v in uvs)
    inV = sorted(v for u, v in uvs)
    vMode = classify(inV, outV, extent=4.0)
    uMode = classify(inU, outU, extent=3.0)

    cacheFile.write_text(f"{vMode},{uMode}")
    print(f"uv probe: v={vMode} u={uMode} (exported u={outU} v={outV})")
    return {"v": vMode, "u": uMode}


def fileSizeHuman(path: Path) -> str:
    size = path.stat().st_size
    for unit in ("B", "K", "M", "G"):
        if size < 1024:
            return f"{size:.0f}{unit}" if unit == "B" else f"{size:.1f}{unit}"
        size /= 1024
    return f"{size:.1f}T"


def run(*args, **kwargs):
    result = subprocess.run([str(a) for a in args], **kwargs)
    if result.returncode != 0:
        print(f"Command failed: {' '.join(str(a) for a in args)}", file=sys.stderr)
        sys.exit(1)
    return result


def ensureBaker():
    """Compile ktx2bc7 (UASTC → BC7 baker) if missing or stale."""
    if BAKER.exists() and BAKER.stat().st_mtime >= BAKER_SRC.stat().st_mtime:
        return
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    run("gcc", "-O2", BAKER_SRC,
        f"-I{KTX_ROOT / 'include'}", f"-L{KTX_ROOT / 'lib'}", "-lktx",
        f"-Wl,-rpath,{KTX_ROOT / 'lib'}", "-o", BAKER)
    print("ktx2bc7: built")


def bakeBc7(path: Path):
    """Transcode a UASTC KTX2 in place to raw BC7 blocks. No-op if the file
    is already baked (vkFormat BC7_UNORM_BLOCK=145 / BC7_SRGB_BLOCK=146)."""
    with open(path, "rb") as f:
        f.seek(12)
        vkFormat = int.from_bytes(f.read(4), "little")
    if vkFormat in (145, 146):
        return

    ensureBaker()
    baked = path.with_suffix(".ktx2.bc7")
    run(BAKER, path, baked)
    os.replace(baked, path)


# ═══════════════════════════════════════════════════════════════════════════
#  Blender export mode
# ═══════════════════════════════════════════════════════════════════════════
def collectSplatDataFromObject(obj):
    """Walk material node groups labelled *splat*: SplatColor → weight-map
    image name, red/green/blue/alpha inputs → detail style image names."""
    colorInputs = ["red", "green", "blue", "alpha"]
    outputData = {}

    if not obj.data or not hasattr(obj.data, "materials"):
        return outputData

    for mat in obj.data.materials:
        if not mat or not mat.use_nodes:
            continue

        for node in mat.node_tree.nodes:
            if node.type != "GROUP" or "splat" not in node.label.lower():
                continue

            splatColorInput = node.inputs.get("SplatColor")
            if splatColorInput is None or not splatColorInput.links:
                continue
            label = splatColorInput.links[0].from_node.image.name

            outputData[label] = {}
            for inputName in colorInputs:
                inp = next((s for s in node.inputs if s.name.lower() == inputName), None)

                imageName = None
                if inp and inp.links:
                    frm = inp.links[0].from_node
                    if frm.type == "TEX_IMAGE" and frm.image:
                        imageName = frm.image.name

                outputData[label][inputName] = imageName

    return outputData


def blenderExport():
    argv = sys.argv
    argv = argv[argv.index("--") + 1:]
    outfile = argv[0]
    splatInfoOutfile = argv[1] if len(argv) > 1 else None

    exportCollection = bpy.data.collections.get("export")
    if not exportCollection:
        print("Export collection not found.")
        sys.exit(1)

    terrainObjects = [o for o in exportCollection.objects if "terrain" in o.name.lower()]
    if not terrainObjects:
        print("No terrain objects found.")
        sys.exit(1)

    mergedSplatInfo = {}
    for terrain in terrainObjects:
        splatData = collectSplatDataFromObject(terrain)
        if splatData:
            terrain["splatInfo"] = splatData
            mergedSplatInfo.update(splatData)

    if splatInfoOutfile:
        Path(splatInfoOutfile).parent.mkdir(parents=True, exist_ok=True)
        with open(splatInfoOutfile, "w", encoding="utf-8") as f:
            json.dump(mergedSplatInfo, f, indent=2)
        print(json.dumps(mergedSplatInfo, indent=2))

    bpy.ops.export_scene.gltf(filepath=outfile,
                              export_format="GLB",
                              export_yup=True,
                              export_extras=True,
                              export_normals=True,
                              export_texcoords=True,
                              export_tangents=True,
                              export_materials="EXPORT",
                              export_image_format="NONE",
                              export_unused_images=False,
                              use_active_scene=True,
                              export_apply=False,
                              collection="export")


# ═══════════════════════════════════════════════════════════════════════════
#  Style texture merge (albedo.rgb+rough / normal.xy+AO+disp → 2K KTX2)
# ═══════════════════════════════════════════════════════════════════════════
def buildTextureIndex():
    """filename (lowercase) → full path, over TEXTURE_ROOT. Skips the
    downscaled 256/512/1024 copies that ship inside the 4k sets."""
    index = {}
    for p in TEXTURE_ROOT.rglob("*"):
        if not p.is_file() or p.suffix.lower() not in (".png", ".jpg", ".jpeg", ".exr"):
            continue
        if p.parent.name in ("256", "512", "1024"):
            continue
        index.setdefault(p.name.lower(), p)
    return index


def stripBlenderSuffix(name):
    return re.sub(r"\.\d{3}$", "", name)


def detectType(filename):
    n = filename.lower()
    if any(k in n for k in ("color", "basecolor", "diffuse", "albedo", "diff")):
        return "diffuse"
    if any(k in n for k in ("normalgl", "normal", "nor")):
        return "normal"
    if any(k in n for k in ("roughness", "rough", "smoothness")):
        return "roughness"
    if any(k in n for k in ("displacement", "disp", "height")):
        return "displacement"
    if any(k in n for k in ("ao", "ambientocclusion", "occlusion")):
        return "ao"
    return None


def loadImage(filepath):
    filepath = str(filepath)
    if filepath.lower().endswith(".exr"):
        import numpy as np
        import OpenEXR
        f = OpenEXR.File(filepath)
        channels = f.channels()
        key = list(channels.keys())[0]
        pixels = channels[key].pixels  # float32 (H, W, C)
        pixels = np.clip(pixels, 0.0, 1.0)
        pixels = (pixels * 255.0 + 0.5).astype("uint8")
        from PIL import Image
        if pixels.shape[2] == 1:
            return Image.fromarray(pixels[:, :, 0], mode="L")
        if pixels.shape[2] == 3:
            return Image.fromarray(pixels, mode="RGB")
        return Image.fromarray(pixels[:, :, :4], mode="RGBA")
    from PIL import Image
    return Image.open(filepath)


def packStyle(sourcePath: Path, outDir: Path):
    """Merge a style's source maps into albedo.png + normal.png (2K) then
    convert both to UASTC KTX2. Returns (albedo, normal) relative pak paths."""
    from PIL import Image

    textures = {}
    for f in sorted(sourcePath.parent.iterdir()):
        if f.is_file() and f.suffix.lower() in (".png", ".jpg", ".jpeg", ".exr"):
            t = detectType(f.name)
            if t:
                textures[t] = f

    if "diffuse" not in textures or "normal" not in textures:
        raise RuntimeError(f"style {sourcePath.parent.name}: missing diffuse/normal "
                           f"(found: {sorted(textures)})")

    def gray(key, fallback):
        if key in textures:
            return loadImage(textures[key]).convert("L")
        print(f"  [warn] {sourcePath.parent.name}: no {key} map, using {fallback}")
        return Image.new("L", (16, 16), fallback)

    diffuse = loadImage(textures["diffuse"]).convert("RGB")
    normal = loadImage(textures["normal"]).convert("RGB")
    rough = gray("roughness", 255)
    ao = gray("ao", 255)
    disp = gray("displacement", 128)

    diffuse = diffuse.resize((STYLE_RES, STYLE_RES), Image.LANCZOS)
    normal = normal.resize((STYLE_RES, STYLE_RES), Image.LANCZOS)
    rough = rough.resize((STYLE_RES, STYLE_RES), Image.BILINEAR)
    ao = ao.resize((STYLE_RES, STYLE_RES), Image.BILINEAR)
    disp = disp.resize((STYLE_RES, STYLE_RES), Image.BILINEAR)

    outDir.mkdir(parents=True, exist_ok=True)
    albedoPng = outDir / "albedo.png"
    normalPng = outDir / "normal.png"

    r, g, b = diffuse.split()
    Image.merge("RGBA", (r, g, b, rough)).save(albedoPng)
    nx, ny, nz = normal.split()
    Image.merge("RGBA", (nx, ny, ao, disp)).save(normalPng)

    toktxEnv = {**os.environ, "LD_LIBRARY_PATH": TOKTX_LIB_PATH}
    base = ["--genmipmap", "--2d", "--target_type", "RGBA",
            "--encode", "uastc", "--zcmp", "19"]

    run(TOKTX, *base, "--assign_oetf", "srgb", "--assign_primaries", "srgb",
        outDir / "albedo.ktx2", albedoPng, env=toktxEnv)
    run(TOKTX, *base, "--assign_oetf", "linear", "--assign_primaries", "none",
        outDir / "normal.ktx2", normalPng, env=toktxEnv)

    bakeBc7(outDir / "albedo.ktx2")
    bakeBc7(outDir / "normal.ktx2")

    albedoPng.unlink()
    normalPng.unlink()


# ═══════════════════════════════════════════════════════════════════════════
#  Standalone pipeline
# ═══════════════════════════════════════════════════════════════════════════
def newestMtime(splatInfoPath: Path, textureIndex: dict, splatInfo: dict) -> int:
    mtime = BLEND_FILE.stat().st_mtime_ns
    blendDir = BLEND_FILE.parent

    for groupName, groupValue in splatInfo.items():
        groupDir = blendDir / groupName
        if groupDir.is_dir():
            for png in groupDir.rglob("*.png"):
                mtime = max(mtime, png.stat().st_mtime_ns)
        for channelName in (groupValue or {}).values():
            if not channelName:
                continue
            src = textureIndex.get(stripBlenderSuffix(channelName).lower())
            if src:
                mtime = max(mtime, src.stat().st_mtime_ns)

    mtime = max(mtime, THIS_SCRIPT.stat().st_mtime_ns)
    if splatInfoPath.exists():
        mtime = max(mtime, splatInfoPath.stat().st_mtime_ns)
    return mtime


def convertSplatTiles(splatInfo: dict):
    """Each painted UDIM tile → one UASTC KTX2 layer file. Returns per-group
    tile lists in layer order (layer index = list position, globally offset).
    Only v=one-minus / u=same orientation is supported (asserted by the
    caller): exported in-tile V is GPU(top-row)-correct, so no flips here."""
    blendDir = BLEND_FILE.parent
    toktxEnv = {**os.environ, "LD_LIBRARY_PATH": TOKTX_LIB_PATH}
    groupsOut = []
    globalLayer = 0

    for groupName, channels in splatInfo.items():
        groupDir = blendDir / groupName
        if not groupDir.is_dir():
            print(f"[warn] splat group '{groupName}' has no directory {groupDir}")
            continue

        outGroupDir = OUT_TEX_DIR / "splat" / groupName
        tiles = []
        for png in sorted(groupDir.glob("*.png")):
            udim = int(png.stem.split(".")[-1])
            if png.stat().st_size <= NOOP_MAX_SIZE:
                stale = outGroupDir / f"{udim}.ktx2"
                if stale.exists():
                    stale.unlink()
                continue

            outGroupDir.mkdir(parents=True, exist_ok=True)
            dst = outGroupDir / f"{udim}.ktx2"
            if not dst.exists() or dst.stat().st_mtime < png.stat().st_mtime:
                run(TOKTX, "--genmipmap", "--2d", "--target_type", "RGBA",
                    "--encode", "uastc", "--zcmp", "19",
                    "--assign_oetf", "linear", "--assign_primaries", "none",
                    dst, png, env=toktxEnv)
            bakeBc7(dst)

            tiles.append({
                "udim": udim,
                "file": str(dst.relative_to(PAK_DIR)),
                "layer": globalLayer,
            })
            globalLayer += 1

        groupsOut.append({"name": groupName, "channels": channels, "tiles": tiles})
        print(f"splat {groupName}: {len(tiles)} painted tiles")

    return groupsOut, globalLayer


def convertStyles(splatInfo: dict, textureIndex: dict):
    """Unique channel textures → style layers. Returns (styles, styleRemap)."""
    styles = []          # [{name, albedo, normal}]
    styleIndex = {}      # texture name → layer index
    remap = {}

    for groupName, channels in splatInfo.items():
        remap[groupName] = {}
        for channel in ("red", "green", "blue", "alpha"):
            texName = channels.get(channel)
            if not texName:
                remap[groupName][channel] = -1
                continue

            if texName in styleIndex:
                remap[groupName][channel] = styleIndex[texName]
                continue

            srcFile = textureIndex.get(stripBlenderSuffix(texName).lower())
            if not srcFile:
                raise RuntimeError(f"style texture '{texName}' not found under {TEXTURE_ROOT}")

            idx = len(styles)
            styleDir = OUT_TEX_DIR / "styles" / f"{idx:02d}"
            print(f"style {idx}: {texName} ({srcFile.parent.name})")
            packStyle(srcFile, styleDir)

            styleIndex[texName] = idx
            remap[groupName][channel] = idx
            styles.append({
                "name": texName,
                "albedo": str((styleDir / "albedo.ktx2").relative_to(PAK_DIR)),
                "normal": str((styleDir / "normal.ktx2").relative_to(PAK_DIR)),
            })

    return styles, remap


def buildModel():
    OUT_MODEL_DIR.mkdir(parents=True, exist_ok=True)
    glb = TMP_DIR / "oghuzlands.blend.glb"
    chunked = TMP_DIR / "oghuzlands.chunked.glb"
    packed = OUT_MODEL_DIR / "oghuzlands.glb"

    splatInfoPath = TMP_DIR / "oghuzlands.terrain-splatinfo.json"

    print("──────── blender → glb")
    run("blender", str(BLEND_FILE), "--background",
        "--python", str(THIS_SCRIPT), "--", str(glb), str(splatInfoPath))
    print(f"  {fileSizeHuman(glb)}")

    print("──────── terrain-chunker")
    run(TERRAIN_CHUNKER, glb, chunked, CHUNK_GRID, CHUNK_GRID)

    print("──────── gltfpack")
    # -vtf keeps texcoords float: integer texcoord quantization (-vt) clamps
    # to [0,1], destroying the 0..10 UDIM range, and drops the payload
    # entirely when no material references TEXCOORD_0 (chunker writes none).
    # No -cc: meshopt stream compression would apply its EXPONENTIAL filter
    # to the float POSITION/TEXCOORD streams, and filament bundles meshopt
    # 0.18 while this gltfpack is built from 1.0 — the exp format changed
    # between the two, so filament's decoder silently mangles the values
    # (UV V collapsed from [-9,1] to [-1,1]).
    run(GLTFPACK, "-vpf", "-vn", "16", "-vtf", "-kn", "-kv", "-ke",
        "-i", chunked, "-o", packed)
    glb.unlink()
    chunked.unlink()
    print(f"  {fileSizeHuman(packed)}")

    return packed, splatInfoPath


def pipelineMain():
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    splatInfoPath = TMP_DIR / "oghuzlands.terrain-splatinfo.json"

    # model rebuild when missing or the .blend changed (tile/style edits don't)
    packed = OUT_MODEL_DIR / "oghuzlands.glb"
    needModel = (not splatInfoPath.exists() or not packed.exists() or
                 packed.stat().st_mtime < BLEND_FILE.stat().st_mtime)
    if needModel:
        packed, splatInfoPath = buildModel()

    with open(splatInfoPath, "r", encoding="utf-8") as f:
        splatInfo = json.load(f)

    textureIndex = buildTextureIndex()

    stampFile = TMP_DIR / "oghuzlands.stamp"
    mtime = newestMtime(splatInfoPath, textureIndex, splatInfo)
    if stampFile.exists() and stampFile.read_text().strip() == str(mtime) and \
            (OUT_MODEL_DIR / "oghuzlands.json").exists():
        print("terrain assets up to date")
        return

    orient = probeUvOrientation()
    if orient != {"v": "one-minus", "u": "same"}:
        raise RuntimeError(
            f"unsupported UV orientation {orient}: terrain.mat only implements "
            "v=one-minus/u=same (Blender 5.x exporter behavior)")

    groups, layerCount = convertSplatTiles(splatInfo)
    styles, remap = convertStyles(splatInfo, textureIndex)

    manifest = {
        "model": str(packed.relative_to(PAK_DIR)),
        "material": "materials/terrain.filamat",
        "chunkGrid": CHUNK_GRID,
        "udimGrid": UDIM_GRID,
        "styleTiling": 0.5,
        "sandHeight": 30,
        "sandFade": 20,
        "snowHeight": 900,
        "snowFade": 150,
        "cliffSlope": 0.32,
        "cliffFade": 0.12,
        "groups": [
            {
                "name": g["name"],
                "tiles": g["tiles"],
                "channels": [remap[g["name"]][c] for c in ("red", "green", "blue", "alpha")],
            }
            for g in groups
        ],
        "styles": styles,
    }

    manifestPath = OUT_MODEL_DIR / "oghuzlands.json"
    with open(manifestPath, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    stampFile.write_text(str(mtime))
    print("──────── done")
    print(f"  model:     {fileSizeHuman(packed)}")
    print(f"  manifest:  {manifestPath}")
    print(f"  splat layers: {layerCount}, styles: {len(styles)}")


if __name__ == "__main__":
    if _INSIDE_BLENDER:
        blenderExport()
    else:
        pipelineMain()
