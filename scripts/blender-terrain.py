#!/usr/bin/env python3
"""
Blender terrain asset pipeline: .blend -> chunked .glb -> packed .glb ->
.zstd model + Jolt shapes sidecar + used UDIM splat images + detail textures.

Per file:  blender -> terrain-chunker -> gltfpack -> jolt-shape-builder -> zstd
           + splat UDIM ktx2 (used tiles only) + detail albedo/normal ktx2

Outputs (c-game/data/pak_1):
    models/terrain/<stem>.zstd                  zstd(packed chunked glb)
    models/terrain/<stem>.jolt.zstd             zstd(Jolt shapes sidecar)
    models/terrain/<stem>/<group>/<group>.<udim>.ktx2   used splat weight tiles
    images/terrain/<detailName>/albedo.ktx2                     1024 sRGB
    images/terrain/<detailName>/normal.ktx2                     1024 linear

GLB notes:
    - gltfpack runs WITHOUT -cc: Diligent's GLTF loader has no meshopt
      buffer support (reads EXT_meshopt_compression views as garbage);
      the zstd pass keeps the shipped size small instead.
    - node extras carry the splatInfo map (splat group -> per-channel detail
      texture names) and the rigidBodyShape MESH tag per terrain chunk, so
      the engine resolves splat textures and physics shapes from the model
      itself.

Standalone mode is invoked as:
    python3 scripts/blender-terrain.py [--blend <file>]... [--force]
Inside Blender it is invoked by the standalone mode as:
    blender <file> --background --python scripts/blender-terrain.py -- <glb> <splatinfo.json> <detailpaths.json>
"""

import json
import os
import sys

try:
    import bpy        # noqa: F401 - available only inside Blender
    _INSIDE_BLENDER = True
except ImportError:
    _INSIDE_BLENDER = False


# ═══════════════════════════════════════════════════════════════════════════
#  BLENDER EXPORT MODE
# ═══════════════════════════════════════════════════════════════════════════
def blenderExport():
    argv = sys.argv
    argv = argv[argv.index("--") + 1:]
    outfile = argv[0]
    splatInfoOutfile = argv[1] if len(argv) > 1 else None
    detailPathsOutfile = argv[2] if len(argv) > 2 else None

    blenderExportTextured(outfile, splatInfoOutfile, detailPathsOutfile)


def saveRigidBodyExtras(exportCollection):
    for obj in exportCollection.all_objects:
        if obj.rigid_body:
            rb = obj.rigid_body
            obj["rigidBodyShape"]       = rb.collision_shape
            obj["rigidBodyType"]        = rb.type
            obj["rigidBodyMass"]        = rb.mass
            obj["rigidBodyFriction"]    = rb.friction
            obj["rigidBodyRestitution"] = rb.restitution
            print(f"Saved rigid body for '{obj.name}': "
                  f"shape={rb.collision_shape} type={rb.type} "
                  f"mass={rb.mass:.2f} friction={rb.friction:.2f} "
                  f"restitution={rb.restitution:.2f}")


def resolveImageFilepath(img):
    fp = img.filepath
    if not fp:
        return None
    if fp.startswith("//"):
        fp = os.path.join(os.path.dirname(bpy.data.filepath), fp[2:])
    fp = os.path.normpath(os.path.expanduser(fp))
    return fp if os.path.isfile(fp) else None


def collectSplatDataFromObject(obj):
    """Returns (splatInfo, detailPaths).

    splatInfo: {splatUdimImageName: {"red": detailImgName, ...}} - embedded as
    glTF node extras (old engine's format, renderer reads it from the model).
    detailPaths: {detailImgName: absoluteSourceFilepath} - side channel for
    the standalone pipeline's detail ktx2 conversion (NOT embedded).
    """
    colorInputs = ["red", "green", "blue", "alpha"]
    splatInfo = {}
    detailPaths = {}

    if not obj.data or not hasattr(obj.data, "materials"):
        return splatInfo, detailPaths

    for mat in obj.data.materials:
        if not mat or not mat.use_nodes:
            continue

        for node in mat.node_tree.nodes:
            if node.type == 'GROUP' and "splat" in node.label.lower():
                splatColorInput = node.inputs["SplatColor"]
                if not splatColorInput.links:
                    continue
                fromNode = splatColorInput.links[0].from_node
                if fromNode.type != 'TEX_IMAGE' or not fromNode.image:
                    continue
                label = fromNode.image.name

                splatInfo[label] = {}
                for inputName in colorInputs:
                    inp = next(
                        (s for s in node.inputs if s.name.lower() == inputName),
                        None
                    )

                    imageName = None
                    if inp and inp.links:
                        frm = inp.links[0].from_node
                        if frm.type == 'TEX_IMAGE' and frm.image:
                            imageName = frm.image.name
                            if imageName not in detailPaths:
                                detailPaths[imageName] = resolveImageFilepath(frm.image) or ""

                    splatInfo[label][inputName] = imageName

    return splatInfo, detailPaths


def blenderExportTextured(outfile, splatInfoOutfile=None, detailPathsOutfile=None):
    exportCollection = bpy.data.collections.get("export")
    if not exportCollection:
        print("Export collection not found.")
        sys.exit(1)

    terrainObjects = [
        obj for obj in exportCollection.objects
        if "terrain" in obj.name.lower()
    ]

    if not terrainObjects:
        print("No terrain objects found.")
        sys.exit(1)

    mergedSplatInfo = {}
    mergedDetailPaths = {}

    for terrain in terrainObjects:
        splatData, detailPaths = collectSplatDataFromObject(terrain)
        if splatData:
            terrain["splatInfo"] = splatData
            mergedSplatInfo.update(splatData)
            mergedDetailPaths.update(detailPaths)

    for name, path in mergedDetailPaths.items():
        if not path:
            print(f"WARNING: splat detail image '{name}' has no resolvable file path")

    if splatInfoOutfile:
        with open(splatInfoOutfile, "w", encoding="utf-8") as f:
            json.dump(mergedSplatInfo, f, indent=2)

        vegetationGroups = [name for name in mergedSplatInfo.keys()
                            if isVegetationGroupName(name)]
        vegetationGroupsPath = os.path.join(
            os.path.dirname(splatInfoOutfile),
            os.path.basename(splatInfoOutfile).replace(
                "terrain-splatinfo", "terrain-vegetation-groups"))
        with open(vegetationGroupsPath, "w", encoding="utf-8") as f:
            json.dump(vegetationGroups, f, indent=2)

    if detailPathsOutfile:
        with open(detailPathsOutfile, "w", encoding="utf-8") as f:
            json.dump(mergedDetailPaths, f, indent=2)

    saveRigidBodyExtras(exportCollection)

    bpy.ops.export_scene.gltf(filepath=outfile,
                              export_format="GLB",
                              export_yup=True,
                              export_extras=True,
                              export_normals=True,
                              export_texcoords=True,
                              export_tangents=True,
                              export_materials='EXPORT',
                              export_image_format="NONE",
                              export_unused_images=False,
                              use_active_scene=True,
                              export_apply=False,
                              collection="export")


# ═══════════════════════════════════════════════════════════════════════════
#  STANDALONE PIPELINE MODE
# ═══════════════════════════════════════════════════════════════════════════
import shutil
import subprocess
import tempfile
from pathlib import Path

THIRDPARTY = Path("/home/enes/Projects/c/cpp-thirdparty")
GLTFPACK = THIRDPARTY / "meshoptimizer/git/build-linux/gltfpack"
TOKTX = Path("/home/enes/Sdks/ktx-4.4.2/bin/toktx")
KTX_LD_LIBRARY_PATH = "/home/enes/Sdks/ktx-4.4.2/lib/"
BLENDER_BIN = "blender"

ROOT = Path(__file__).resolve().parent.parent
TOOLS_DIR = ROOT / "tools"
TERRAIN_CHUNKER = TOOLS_DIR / "terrain-chunker" / "terrain-chunker"
JOLT_SHAPE_BUILDER = TOOLS_DIR / "jolt-shape-builder" / "jolt-shape-builder"

BLEND_FILES = [
    Path("/home/enes/Projects/assets/Scenes/Terrain/oghuzlands/oghuzlands.blend"),
]

OUTPUT_DIR = ROOT / "c-game/data/pak_1/models/terrain"
IMAGES_DIR = ROOT / "c-game/data/pak_1/images/terrain"

CHUNK_GRID_X = 4
CHUNK_GRID_Y = 4
DETAIL_MAX_SIZE = 1024

THIS_SCRIPT = Path(__file__).resolve()


def fileSizeHuman(path: Path) -> str:
    size = path.stat().st_size
    for unit in ("B", "K", "M", "G"):
        if size < 1024:
            return f"{size:.0f}{unit}" if unit == "B" else f"{size:.1f}{unit}"
        size /= 1024
    return f"{size:.1f}T"


def run(*args, **kwargs):
    result = subprocess.run(args, **kwargs)
    if result.returncode != 0:
        print(f"Command failed: {' '.join(str(a) for a in args)}", file=sys.stderr)
        sys.exit(1)
    return result


def cachedSplatInfoPath(blendFile: Path, scriptsTmp: Path) -> Path:
    return scriptsTmp / f"{blendFile.stem}.terrain-splatinfo.json"


def cachedDetailPathsPath(blendFile: Path, scriptsTmp: Path) -> Path:
    return scriptsTmp / f"{blendFile.stem}.terrain-detailpaths.json"


def loadJson(path: Path):
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def loadSplatInfo(path: Path) -> dict:
    data = loadJson(path)
    return data if isinstance(data, dict) else {}


def loadDetailPaths(path: Path) -> dict:
    data = loadJson(path)
    return data if isinstance(data, dict) else {}


def isVegetationGroupName(name: str) -> bool:
    return name.lower().startswith("grass")


def ensureTool(binary: Path, buildSh: Path) -> Path:
    run(str(buildSh))
    if not binary.exists():
        print(f"tool build failed: {binary}", file=sys.stderr)
        sys.exit(1)
    return binary


def newestMtime(blendFile: Path, scriptsTmp: Path) -> int:
    mtime = blendFile.stat().st_mtime_ns
    blendDir = blendFile.parent

    splatInfo = loadSplatInfo(cachedSplatInfoPath(blendFile, scriptsTmp))
    for groupName in splatInfo:
        groupDir = blendDir / groupName
        if not groupDir.is_dir():
            continue
        for png in groupDir.rglob("*.png"):
            mtime = max(mtime, png.stat().st_mtime_ns)

    for filePath in loadDetailPaths(cachedDetailPathsPath(blendFile, scriptsTmp)).values():
        if isinstance(filePath, str) and filePath and Path(filePath).is_file():
            mtime = max(mtime, Path(filePath).stat().st_mtime_ns)

    return mtime


def needsConvert(blendFile: Path, scriptsTmp: Path, force: bool) -> bool:
    if force:
        return True
    stampFile = scriptsTmp / blendFile.name
    mtime = newestMtime(blendFile, scriptsTmp)

    if stampFile.exists():
        saved = stampFile.read_text().strip()
        if saved == str(mtime):
            return False

    stampFile.write_text(str(mtime))
    return True


def convertSplatTile(png: Path, outDir: Path, oetf: str, primaries: str):
    outDir.mkdir(parents=True, exist_ok=True)
    ktx2 = outDir / f"{png.stem}.ktx2"
    env = {**os.environ, "LD_LIBRARY_PATH": KTX_LD_LIBRARY_PATH}
    run(str(TOKTX), "--genmipmap", "--2d",
        "--assign_oetf", oetf,
        "--assign_primaries", primaries,
        "--zcmp", "19",
        str(ktx2), str(png),
        env=env)
    return ktx2


def isNoopSplatPng(pngPath: Path) -> bool:
    NOOP_MAX_SIZE = 20_800
    return pngPath.stat().st_size <= NOOP_MAX_SIZE


def convertSplatDirs(blendFile: Path, splatInfoJson: Path):
    stem = blendFile.stem
    splatInfo = loadSplatInfo(splatInfoJson)
    if not splatInfo:
        return

    blendDir = blendFile.parent

    for groupName in splatInfo:
        groupDir = blendDir / groupName
        if not groupDir.is_dir():
            print(f"WARNING: splat group '{groupName}' has no directory at {groupDir}")
            continue

        outDir = OUTPUT_DIR / stem / groupName
        pngs = sorted(groupDir.glob("*.png"))
        if not pngs:
            continue

        activePngs = [p for p in pngs if not isNoopSplatPng(p)]
        noopStems = [p.stem for p in pngs if isNoopSplatPng(p)]

        print(f"splat textures: {groupName} -> {outDir} "
              f"({len(activePngs)} used, {len(noopStems)} skipped)")

        if outDir.is_dir():
            for noopStem in noopStems:
                stale = outDir / f"{noopStem}.ktx2"
                if stale.exists():
                    stale.unlink()
                    print(f"  removed stale: {stale.name}")

        for png in activePngs:
            ktx2 = convertSplatTile(png, outDir, "linear", "none")
            print(f"  {ktx2.name}: {fileSizeHuman(ktx2)}")


def findNormalMap(albedoPath: Path) -> Path:
    name = albedoPath.name
    candidates = [
        name.replace("BaseColor", "Normal"),
        name.replace("Albedo", "Normal").replace("albedo", "Normal"),
        name.replace("diff", "nor_gl"),
    ]
    for cand in candidates:
        if cand == name:
            continue
        candPath = albedoPath.parent / cand
        if candPath.is_file():
            return candPath
    return None


def findRoughnessMap(albedoPath: Path) -> Path:
    name = albedoPath.name
    candidates = [
        name.replace("BaseColor", "Roughness"),
        name.replace("Albedo", "Roughness"),
        name.replace("diff", "rough"),
    ]
    for cand in candidates:
        if cand == name:
            continue
        candPath = albedoPath.parent / cand
        if candPath.is_file():
            return candPath
    return None


def findDisplacementMap(albedoPath: Path):
    # Height source for the POM alpha channel (old-engine convention: the
    # height lives in the normal map's alpha). The sets mix polyhaven-style
    # *_disp/*.jpg names, polyhaven 4k jpgs (*_Displacement.jpg, *_Bump.jpg
    # fallback) and mat-test exrs (*_Displacement.exr).
    name = albedoPath.name
    stem, _, ext = name.rpartition(".")
    candidates = []
    if "diff" in name:
        candidates.append(name.replace("diff", "disp"))
    if "BaseColor" in name:
        candidates.append(name.replace("BaseColor", "Displacement"))
        candidates.append(name.replace("BaseColor", "Bump"))
    if "Albedo" in name:
        candidates.append(stem.replace("Albedo", "Displacement") + ".exr")
        candidates.append(name.replace("Albedo", "Displacement"))
    for cand in candidates:
        candPath = albedoPath.parent / cand
        if cand != name and candPath.is_file():
            return candPath
    return None


def loadGrayscaleL8(path: Path) -> "Image.Image":
    from PIL import Image
    import numpy as np
    img = Image.open(path)
    if img.mode in ("I;16", "I;16L", "I;16B", "I"):
        data = np.asarray(img).astype(np.uint32)
        if img.mode == "I":
            lo, hi = float(data.min()), float(data.max())
            data = ((data - lo) * (255.0 / (hi - lo))).round() if hi > lo else data * 0.0
        else:
            data = data >> 8
        return Image.fromarray(data.astype(np.uint8), "L")
    return img.convert("L")


def loadHeightMap(path: Path):
    # Returns an 8-bit grayscale PIL image. EXR (float, possibly linear
    # meters-ish range) is min-max normalized to the full [0, 255] range —
    # the height field is relative anyway, the shader's heightScale sets the
    # absolute depth.
    from PIL import Image
    if path.suffix.lower() == ".exr":
        import numpy as np
        data = None
        try:
            import cv2
            data = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
        except ImportError:
            pass
        if data is not None:
            if data.ndim == 3:
                data = data[..., 0]
            lo, hi = float(data.min()), float(data.max())
            if hi <= lo:
                raise RuntimeError(f"degenerate displacement: {path}")
            data = (data - lo) / (hi - lo)
            return Image.fromarray((data * 255.0).round().clip(0, 255).astype("uint8"), "L")
        with tempfile.TemporaryDirectory() as t:
            png = Path(t) / "disp.png"
            run("magick", str(path), "-auto-level", "-colorspace", "Gray", "-depth", "8", str(png))
            return Image.open(png).convert("L")
    return loadGrayscaleL8(path)


def convertDetailTexture(srcPath: Path, outDir: Path, kind: str, dispPath: Path = None,
                         roughPath: Path = None):
    from PIL import Image
    oetf, primaries = ("srgb", "srgb") if kind == "albedo" else ("linear", "none")
    outDir.mkdir(parents=True, exist_ok=True)
    ktx2 = outDir / f"{kind}.ktx2"

    with tempfile.TemporaryDirectory() as tmp:
        tmpPath = Path(tmp)
        png = tmpPath / f"{kind}.png"
        img = Image.open(srcPath)
        if max(img.size) > DETAIL_MAX_SIZE:
            img = img.resize((DETAIL_MAX_SIZE, DETAIL_MAX_SIZE), Image.LANCZOS)
        img = img.convert("RGBA")
        if kind == "albedo":
            if roughPath is None:
                raise RuntimeError(
                    f"no roughness map for {srcPath} — albedo alpha (roughness) would be opaque"
                )
            rough = loadGrayscaleL8(roughPath).resize(img.size, Image.LANCZOS)
            img.putalpha(rough)
        if kind == "normal":
            if dispPath is None:
                raise RuntimeError(f"no displacement map for {srcPath} — POM height would be flat")
            disp = loadHeightMap(dispPath).resize(img.size, Image.LANCZOS)
            img.putalpha(disp)
        img.save(png)
        convertSplatTile(png, outDir, oetf, primaries)

    return ktx2


def convertDetailTextures(detailPathsJson: Path):
    detailPaths = loadDetailPaths(detailPathsJson)
    if not detailPaths:
        return

    for detailName, srcPath in detailPaths.items():
        if not srcPath:
            continue
        src = Path(srcPath)
        if not src.is_file():
            print(f"WARNING: detail source missing: {src}")
            continue

        outDir = IMAGES_DIR / detailName
        rough = findRoughnessMap(src)
        if rough is None:
            print(f"WARNING: no roughness map found for {detailName} ({src})")
        albedo = convertDetailTexture(src, outDir, "albedo", roughPath=rough)
        print(f"detail texture: {detailName} albedo (+rough {rough.name if rough else 'NONE'})"
              f" -> {albedo} ({fileSizeHuman(albedo)})")

        normal = findNormalMap(src)
        if normal is None:
            print(f"WARNING: no normal map found for {detailName} (albedo-only set)")
            stale = outDir / "normal.ktx2"
            if stale.exists():
                stale.unlink()
            continue
        disp = findDisplacementMap(src)
        if disp is None:
            raise RuntimeError(f"no displacement map found for {detailName} ({src})")
        ktx2 = convertDetailTexture(normal, outDir, "normal", disp)
        print(f"detail texture: {detailName} normal (+disp {disp.name}) -> {ktx2} ({fileSizeHuman(ktx2)})")


def convertBlendFile(blendFile: Path, scriptsTmp: Path, force: bool):
    if not blendFile.exists():
        print(f"WARNING: {blendFile} does not exist, skipping.")
        return

    if not needsConvert(blendFile, scriptsTmp, force):
        print(f"up to date: {blendFile.stem}")
        return

    stem = blendFile.stem
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    glb                = OUTPUT_DIR / f"{stem}.glb"
    chunkedGlb         = OUTPUT_DIR / f"{stem}.chunked.glb"
    packedGlb          = OUTPUT_DIR / f"{stem}.packed.glb"
    modelZstd          = OUTPUT_DIR / f"{stem}.zstd"
    joltShapes         = OUTPUT_DIR / f"{stem}.jolt"
    joltShapesZstd     = OUTPUT_DIR / f"{stem}.jolt.zstd"
    splatInfoJson      = cachedSplatInfoPath(blendFile, scriptsTmp)
    detailPathsJson    = cachedDetailPathsPath(blendFile, scriptsTmp)

    print("#############################################")
    print(f"blend to glb {stem}... ", end="", flush=True)

    blenderCommon = [BLENDER_BIN, str(blendFile), "--background",
                     "--python", str(THIS_SCRIPT), "--"]
    run(*blenderCommon, str(glb), str(splatInfoJson), str(detailPathsJson))

    print(fileSizeHuman(glb))

    print("terrain-chunker...")
    run(str(ensureTool(TERRAIN_CHUNKER, TERRAIN_CHUNKER.parent / "build.sh")),
        str(glb), str(chunkedGlb), str(CHUNK_GRID_X), str(CHUNK_GRID_Y))

    glb.unlink()
    print(f"chunked size: {fileSizeHuman(chunkedGlb)}")

    print("gltfpack...")
    run(str(GLTFPACK), "-vpf", "-vn", "16", "-vt", "16", "-vtf", "-kn", "-kv", "-ke",
        "-tj", "32",
        "-i", str(chunkedGlb), "-o", str(packedGlb))

    chunkedGlb.unlink()
    print(f"packed size: {fileSizeHuman(packedGlb)}")

    print("jolt shapes...", end=" ", flush=True)
    run(str(ensureTool(JOLT_SHAPE_BUILDER, JOLT_SHAPE_BUILDER.parent / "build.sh")),
        str(packedGlb), str(joltShapes))
    print(fileSizeHuman(joltShapes))

    print("splat textures...")
    convertSplatDirs(blendFile, splatInfoJson)

    print("detail textures...")
    convertDetailTextures(detailPathsJson)

    print("zstd...")
    run("zstd", "-q", "-19", "--rm", "-f", "-o", str(modelZstd), str(packedGlb))
    if joltShapes.exists():
        run("zstd", "-q", "-19", "--rm", "-f", str(joltShapes))
        Path(str(joltShapes) + ".zst").rename(joltShapesZstd)
        print(f"jolt shapes: {fileSizeHuman(joltShapesZstd)}")

    print(f"model: {fileSizeHuman(modelZstd)}")


def pipelineMain():
    args = sys.argv[1:]
    force = False
    detailsOnly = False
    blendFiles = list(BLEND_FILES)
    i = 0
    while i < len(args):
        if args[i] == "--force":
            force = True
            i += 1
        elif args[i] == "--details-only":
            detailsOnly = True
            i += 1
        elif args[i] == "--blend":
            blendFiles = []
            i += 1
            while i < len(args) and not args[i].startswith("--"):
                blendFiles.append(Path(args[i]))
                i += 1
        else:
            i += 1

    if not (ROOT / "c-game/data").is_dir():
        print("where is c-game/data dir?", file=sys.stderr)
        sys.exit(1)

    scriptsTmp = ROOT / "scripts" / ".tmp"
    scriptsTmp.mkdir(parents=True, exist_ok=True)

    if detailsOnly:
        detailPathsJson = scriptsTmp / "oghuzlands.terrain-detailpaths.json"
        convertDetailTextures(detailPathsJson)
        return

    for blendFile in blendFiles:
        convertBlendFile(blendFile, scriptsTmp, force)


if _INSIDE_BLENDER:
    blenderExport()
else:
    pipelineMain()
