#!/usr/bin/env python3
"""Standardize Mixamo-style character GLBs for standard glTF renderers.

Blender's glTF exporter writes a character whose armature object carries a
90-degree X rotation + 0.01 scale (the rig is authored in centimetres) while
the skinned mesh's POSITION data is already in metres. A standard renderer
then applies the armature transform twice: the character
comes out tipped on its side and 100x too small ("eve was 2cm").

This script rewrites the plain (pre-gltfpack) GLB so the hierarchy is
consistent: the armature node becomes identity, and its full transform A is
pre-multiplied into the armature's DIRECT CHILDREN (and only into the
animation keyframes targeting them). World poses are preserved:

    before:  W(child) = A @ L(child)
    after:   W(child) = I @ (A @ L(child))

Deeper joints are left untouched: the hierarchy applies A exactly once (at
the top), so their world transforms — and therefore the skin's
inverseBindMatrices, which invert the original single-A world transforms —
stay valid. Pre-multiplying A into EVERY joint would re-apply it at every
depth (world scale 0.01^depth), so the bone matrices of all but the topmost
joint would carry scale 0.01^(depth-1) and the whole body would collapse to
a point at the root joint.

The mesh node and all vertex data are left untouched on purpose: the
exporter already pre-multiplied the armature transform into POSITION
(centimetres -> metres), so the mesh node stays identity and the data is
already in scene space.

The old engine's loader tolerated the original layout, so this step only
runs in the new engine's export pipeline (scripts/export-models.sh); the
blend files stay untouched.

Usage: gltf-standardize.py input.glb output.glb
"""
import json
import struct
import sys

# --- minimal 4x4 / quaternion math (column-vector convention: M @ v) ---

def mat_identity():
    m = [0.0] * 16
    m[0] = m[5] = m[10] = m[15] = 1.0
    return m


def mat_mul(a, b):
    out = [0.0] * 16
    for c in range(4):
        for r in range(4):
            s = 0.0
            for k in range(4):
                s += a[k * 4 + r] * b[c * 4 + k]
            out[c * 4 + r] = s
    return out


def mat_from_trs(translation, rotation, scale):
    t = translation or [0.0, 0.0, 0.0]
    q = rotation or [0.0, 0.0, 0.0, 1.0]
    s = scale or [1.0, 1.0, 1.0]
    x, y, z, w = q
    # column-major: column c holds (R0c, R1c, R2c)
    r = [
        1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y), 0,
        2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x), 0,
        2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y), 0,
        0, 0, 0, 1,
    ]
    m = mat_identity()
    m[0] = s[0] * r[0]; m[1] = s[0] * r[1]; m[2] = s[0] * r[2]
    m[4] = s[1] * r[4]; m[5] = s[1] * r[5]; m[6] = s[1] * r[6]
    m[8] = s[2] * r[8]; m[9] = s[2] * r[9]; m[10] = s[2] * r[10]
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]
    return m


def mat_to_quat(r):
    # r: 4x4 column-major rotation part: m_rc = r[c*4+r]
    m00, m01, m02 = r[0], r[4], r[8]
    m10, m11, m12 = r[1], r[5], r[9]
    m20, m21, m22 = r[2], r[6], r[10]
    trace = m00 + m11 + m22
    if trace > 0.0:
        s = (trace + 1.0) ** 0.5 * 2
        w = s / 4
        x = (m21 - m12) / s
        y = (m02 - m20) / s
        z = (m10 - m01) / s
    elif m00 > m11 and m00 > m22:
        s = (m00 - m11 - m22 + 1.0) ** 0.5 * 2
        w = (m21 - m12) / s
        x = s / 4
        y = (m01 + m10) / s
        z = (m02 + m20) / s
    elif m11 > m22:
        s = (m11 - m00 - m22 + 1.0) ** 0.5 * 2
        w = (m02 - m20) / s
        x = (m01 + m10) / s
        y = s / 4
        z = (m12 + m21) / s
    else:
        s = (m22 - m00 - m11 + 1.0) ** 0.5 * 2
        w = (m10 - m01) / s
        x = (m20 + m02) / s
        y = (m10 + m12) / s
        z = s / 4
    return [x, y, z, w]


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ]


def quat_norm(q):
    n = (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) ** 0.5
    return [q[0] / n, q[1] / n, q[2] / n, q[3] / n]


def node_matrix(node):
    if "matrix" in node:
        return list(node["matrix"])
    return mat_from_trs(node.get("translation"), node.get("rotation"), node.get("scale"))


def set_node_trs(node, m):
    # write the 4x4 (column-major) back as glTF TRS fields — animation
    # channels target translation/rotation/scale, so the node must stay TRS
    for k in ("translation", "rotation", "scale", "matrix"):
        node.pop(k, None)
    t = [m[12], m[13], m[14]]
    # column scales of the 3x3 part
    s = [
        (m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) ** 0.5,
        (m[4] * m[4] + m[5] * m[5] + m[6] * m[6]) ** 0.5,
        (m[8] * m[8] + m[9] * m[9] + m[10] * m[10]) ** 0.5,
    ]
    r = [0.0] * 12
    for c in range(3):
        for rr in range(3):
            r[c * 4 + rr] = m[c * 4 + rr] / (s[c] if s[c] > 1e-12 else 1.0)
    node["translation"] = [round(v, 10) for v in t]
    node["rotation"] = [round(v, 10) for v in mat_to_quat(r)]
    node["scale"] = [round(v, 10) for v in s]


def trs_components(node):
    t = node.get("translation", [0.0, 0.0, 0.0])
    q = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    s = node.get("scale", [1.0, 1.0, 1.0])
    return t, q, s


def is_identity(m, eps=1e-5):
    for i in range(16):
        want = 1.0 if i % 5 == 0 else 0.0
        if abs(m[i] - want) > eps:
            return False
    return True


# --- GLB container ---

def read_glb(path):
    with open(path, "rb") as f:
        raw = f.read()
    magic, version, total = struct.unpack_from("<4sII", raw, 0)
    if magic != b"glTF" or version != 2:
        raise SystemExit(f"not a GLB v2: {path}")
    off = 12
    chunks = []
    while off < total:
        clen, ctype = struct.unpack_from("<I4s", raw, off)
        data = raw[off + 8: off + 8 + clen]
        chunks.append((ctype, data))
        off += 8 + clen
    json_data = None
    bin_data = None
    for ctype, data in chunks:
        if ctype == b"JSON":
            json_data = data
        elif ctype == b"BIN\x00":
            bin_data = data
    if json_data is None:
        raise SystemExit("no JSON chunk")
    return json.loads(json_data), bin_data or b""


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


# --- accessor value I/O (componentType FLOAT only) ---

FLOAT = 5126
TYPES = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}


def read_accessor(gltf, bin_data, acc_index):
    acc = gltf["accessors"][acc_index]
    if acc.get("componentType") != FLOAT:
        raise SystemExit(f"accessor {acc_index} is not FLOAT; refusing to touch it")
    bv = gltf["bufferViews"][acc["bufferView"]]
    stride = bv.get("byteStride", 0)
    ncomp = TYPES[acc["type"]]
    count = acc["count"]
    start = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = stride or ncomp * 4
    vals = []
    for i in range(count):
        o = start + i * stride
        vals.append(struct.unpack_from(f"<{ncomp}f", bin_data, o))
    return vals, stride, ncomp


def write_accessor(gltf, bin_data, acc_index, vals):
    acc = gltf["accessors"][acc_index]
    bv = gltf["bufferViews"][acc["bufferView"]]
    ncomp = TYPES[acc["type"]]
    stride = bv.get("byteStride", ncomp * 4) or ncomp * 4
    start = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    out = bytearray(bin_data)
    for i, v in enumerate(vals):
        struct.pack_into(f"<{ncomp}f", out, start + i * stride, *v)
    # keep min/max annotations consistent for downstream tools
    if acc.get("type") == "VEC3":
        mn = [min(x[i] for x in vals) for i in range(3)]
        mx = [max(x[i] for x in vals) for i in range(3)]
        acc["min"] = mn
        acc["max"] = mx
    elif acc.get("type") == "VEC4" and "min" in acc:
        mn = [min(x[i] for x in vals) for i in range(4)]
        mx = [max(x[i] for x in vals) for i in range(4)]
        acc["min"] = mn
        acc["max"] = mx
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: gltf-standardize.py input.glb output.glb")
    gltf, bin_data = read_glb(sys.argv[1])
    nodes = gltf["nodes"]

    changed = 0
    kf = 0
    done = set()
    for skin in gltf.get("skins", []):
        arm_idx = skin.get("skeleton", -1)
        if arm_idx < 0:
            # no explicit skeleton field (Blender omits it): the armature
            # is the node whose descendants include every joint
            joints = set(skin["joints"])
            desc_cache = {}
            def descendants(i):
                if i in desc_cache:
                    return desc_cache[i]
                out = set()
                for c in nodes[i].get("children", []):
                    out.add(c)
                    out |= descendants(c)
                desc_cache[i] = out
                return out
            arm_idx = next((i for i, n in enumerate(nodes)
                            if joints <= descendants(i)), -1)
        if arm_idx < 0:
            continue
        A = node_matrix(nodes[arm_idx])
        if is_identity(A):
            print(f"armature node {arm_idx} already identity; nothing to do")
            continue
        # uniform scale of A (the centimetre factor)
        s = (A[0] * A[0] + A[1] * A[1] + A[2] * A[2]) ** 0.5
        # rotation part of A (drop the scale) -> quaternion qA
        R = [v / s if s > 1e-9 else (1.0 if i % 5 == 0 else 0.0) for i, v in enumerate(A[:12])]
        qA = quat_norm(mat_to_quat(R))

        print(f"armature node {arm_idx} ({nodes[arm_idx].get('name')}): "
              f"scale {s:.4f}, rotation quat {[round(v, 4) for v in qA]}")

        # 1. pre-multiply A into the armature's DIRECT CHILDREN (the top of
        #    the joint chain). A @ L (L = T@R@S) is exactly expressible as
        #    TRS because A carries a uniform scale, so the nodes stay TRS and
        #    their animation channels (translation/rotation/scale) remain
        #    valid. See the module docstring for why deeper joints must NOT
        #    be rewritten. The skinned-mesh holder node is skipped: its
        #    transform cancels exactly in the final skinned position
        #    (W_mesh x (W_mesh^-1 x W_j x IBM) x pos), so leaving it authored
        #    keeps the asset bounding box in metre space.
        tA, qA, sA = trs_components(nodes[arm_idx])
        joint_set = set(skin["joints"])
        top = [c for c in nodes[arm_idx].get("children", [])
               if not ("mesh" in nodes[c] and c not in joint_set)]
        for c in top:
            set_node_trs(nodes[c], mat_mul(A, node_matrix(nodes[c])))
            changed += 1
        # 2. armature node -> identity
        for k in ("translation", "rotation", "scale", "matrix"):
            nodes[arm_idx].pop(k, None)
        # 3. animation keyframes targeting the direct children: world motion
        #    preserved. The exporter dedupes identical keyframes into shared
        #    accessors, so transform each output accessor at most once.
        #    Deeper joints' channels are left untouched (their local
        #    transforms were not rewritten either).
        targets = set(top)
        for anim in gltf.get("animations", []):
            samplers = {i: s["output"] for i, s in enumerate(anim["samplers"])}
            for ch in anim["channels"]:
                tgt = ch["target"]
                node = tgt["node"]
                if node not in targets:
                    continue
                path = tgt["path"]
                out = samplers[ch["sampler"]]
                if out in done:
                    continue
                done.add(out)
                if path == "translation":
                    vals, _, _ = read_accessor(gltf, bin_data, out)
                    new = []
                    for v in vals:
                        new.append((
                            tA[0] + A[0] * v[0] + A[4] * v[1] + A[8] * v[2],
                            tA[1] + A[1] * v[0] + A[5] * v[1] + A[9] * v[2],
                            tA[2] + A[2] * v[0] + A[6] * v[1] + A[10] * v[2],
                        ))
                    bin_data = write_accessor(gltf, bin_data, out, new)
                    kf += len(new)
                elif path == "rotation":
                    vals, _, _ = read_accessor(gltf, bin_data, out)
                    new = [tuple(quat_norm(quat_mul(qA, list(v)))) for v in vals]
                    bin_data = write_accessor(gltf, bin_data, out, new)
                    kf += len(new)
                elif path == "scale":
                    vals, _, _ = read_accessor(gltf, bin_data, out)
                    new = [(v[0] * sA[0], v[1] * sA[1], v[2] * sA[2]) for v in vals]
                    bin_data = write_accessor(gltf, bin_data, out, new)
                    kf += len(new)
        print(f"standardized: {changed} nodes, {kf} keyframe values "
              f"({len(gltf.get('animations', []))} animations)")

    write_glb(sys.argv[2], gltf, bin_data)
    print(f"wrote {sys.argv[2]}")


if __name__ == "__main__":
    main()
