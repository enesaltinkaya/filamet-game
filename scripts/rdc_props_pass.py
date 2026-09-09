#!/usr/bin/env python3
import sys
from collections import Counter, defaultdict

RD_LIB = "/home/enes/Apps/renderdoc/build/lib"


def main():
    cap = sys.argv[1] if len(sys.argv) > 1 else "/tmp/RenderDoc/c-game_frame300.rdc"
    sys.path.insert(0, RD_LIB)
    import renderdoc as rd

    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cf = rd.OpenCaptureFile()
    r = cf.OpenFile(cap, "", None)
    if r.OK() is not True:
        sys.exit("open failed: %s" % r.Message())
    res, ctrl = cf.OpenCapture(rd.ReplayOptions(), None)
    if res.OK() is not True:
        sys.exit("replay failed: %s" % res.Message())

    F = rd.ActionFlags

    def walk(a):
        yield a
        for c in a.children:
            yield from walk(c)

    acts = [a for root in ctrl.GetRootActions() for a in walk(root)]
    acts.sort(key=lambda a: a.eventId)
    groups = [a for a in acts if F.PushMarker in a.flags and a.children]

    toplevel = []
    for g in groups:
        nested = False
        for g2 in groups:
            if g2 is g:
                continue
            stack = list(g2.children)
            while stack:
                c = stack.pop()
                if c.eventId == g.eventId:
                    nested = True
                    break
                stack.extend(c.children)
            if nested:
                break
        if not nested:
            toplevel.append(g)

    def kids_of(g):
        out = []
        stack = list(g.children)
        while stack:
            c = stack.pop()
            out.append(c)
            stack.extend(c.children)
        return out

    def group_of(eid):
        for g in toplevel:
            for c in kids_of(g):
                if c.eventId == eid:
                    return g.customName
        return None

    print("capture:", cap)
    for g in toplevel:
        k = kids_of(g)
        print("pass %-14s eid=%-5d children=%-4d draws=%d" % (
            (g.customName or "?")[:14], g.eventId, len(k), sum(1 for c in k if F.Drawcall in c.flags)))

    LAYOUT = [
        ("grass", 7, [4] * 7),
        ("conifer", 4, [307, 310, 316, 352]),
        ("conifer_far", 1, [195]),
        ("deciduous", 4, [28344] * 4),
        ("deciduous", 4, [9144] * 4),
        ("deciduous_far", 1, [6688]),
        ("deciduous_far", 1, [3152]),
        ("acacia", 3, [292, 292, 268]),
        ("palm", 1, [34]),
        ("cactus", 1, [54]),
        ("dead_tree", 2, [220, 226]),
        ("reed", 1, [15]),
        ("shrub", 4, [169, 196, 193, 193]),
        ("rock", 1, [16]),
        ("flower", 1, [18]),
    ]
    def variant_by_idx(ni):
        for sp, nv, tris in LAYOUT:
            for i in range(nv):
                if tris[i] * 3 == ni:
                    return "%s/%d" % (sp, i)
        return "idx%d" % ni

    def pass_draws(name):
        for g in toplevel:
            if (g.customName or "").strip() == name:
                ds = [c for c in kids_of(g) if F.Drawcall in c.flags]
                ds.sort(key=lambda a: a.eventId)
                return ds
        return []

    for passname in ("props", "shadow"):
        ds = pass_draws(passname)
        print()
        print("%s pass: %d draws" % (passname, len(ds)))
        tot_inst = tot_tri = 0
        sp_inst = Counter()
        sp_tri = Counter()
        rows = []
        for d in ds:
            sp = variant_by_idx(d.numIndices).split("/")[0]
            ntri = d.numIndices // 3 * d.numInstances
            tot_inst += d.numInstances
            tot_tri += ntri
            sp_inst[sp] += d.numInstances
            sp_tri[sp] += ntri
            rows.append((d.eventId, variant_by_idx(d.numIndices), d.numIndices, d.numInstances, ntri))
        for eid, v, ni, ninst, ntri in rows:
            print("  eid=%-5d %-16s idx=%-6d inst=%-6d tris=%d" % (eid, v, ni, ninst, ntri))
        print("  total: draws=%d instances=%d tri load=%d" % (len(ds), tot_inst, tot_tri))
        for sp in sorted(sp_tri, key=lambda s: -sp_tri[s]):
            print("  %-14s inst=%-7d tris=%-9d (%.0f%%)" % (sp, sp_inst[sp], sp_tri[sp], 100.0 * sp_tri[sp] / max(1, tot_tri)))
        if passname == "props":
            props_rows = rows

    print()
    want = {
        "EventGPUDuration": 1,
        "RasterizedPrimitives": 6,
        "SamplesPassed": 7,
        "VSInvocations": 8,
        "PSInvocations": 12,
    }
    results = ctrl.FetchCounters([v for v in want.values()])
    print("counter results: %d" % len(results))
    durs = {}
    rprs = defaultdict(int)
    sps = defaultdict(int)
    for cr in results:
        raw = int(cr.counter)
        if raw == 1:
            durs[cr.eventId] = cr.value.d
        elif raw == 6:
            rprs[cr.eventId] += cr.value.u64
        elif raw == 7:
            sps[cr.eventId] += cr.value.u64
    if durs:
        print()
        print("per-pass event-gpu-time sums (EventGPUDuration counter; values overlap, see notes.md round 2 caveat):")
        pass_sum = defaultdict(float)
        pass_count = Counter()
        for eid, v in durs.items():
            pass_sum[group_of(eid)] += v
            pass_count[group_of(eid)] += 1
        for g in toplevel:
            print("  %-14s %.3f ms events=%d" % (
                (g.customName or "?")[:14], 1000.0 * pass_sum.get(g.customName, 0.0), pass_count.get(g.customName, 0)))
        print("  whole-frame sum %.3f ms" % (1000.0 * sum(durs.values()),))
        if props_rows:
            print()
            print("props draws (gpu ms, rasterized tris, samples passed):")
            for eid, v, ni, ninst, ntri in props_rows:
                print("  eid=%-5d %-16s %8.6f ms rpr=%-9d sps=%-9d" % (
                    eid, v, 1000.0 * durs.get(eid, -1.0), rprs.get(eid, 0), sps.get(eid, 0)))
        if rprs:
            print()
            print("RasterizedPrimitives per pass:")
            ps = defaultdict(int)
            for eid, v in rprs.items():
                ps[group_of(eid)] += v
            for g in toplevel:
                if ps.get(g.customName):
                    print("  %-14s %d" % ((g.customName or "?")[:14], ps[g.customName]))
            print("  frame total %d" % sum(rprs.values()))

    ctrl.Shutdown()
    cf.Shutdown()
    rd.ShutdownReplay()


if __name__ == "__main__":
    main()
