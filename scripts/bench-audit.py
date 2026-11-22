#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""bench-audit.py — v3-vs-v2 deep audit on a bench-v2 reference dump.

For each v7 step:
  - byte-level outputs: byte-EQ / numerical diff (NaN-aware)
  - structured outputs: spatial-match (10 cm tol) features, then per-feature
    compare with normalization for known v3-vs-v2 design differences
      * source label rename: v2 `candidate-concave` ≡ v3 `concave` (and convex)
      * planeIds: v3 attributes coplanar-imported corners to BOTH origin and
        destination plane; v2 only stores origin — v3 superset of v2 is a
        semantic match when the extras come from coplanar imports (named
        with `+P{extra}CP{n}` suffix in v3's merged corner name)

Usage:
  bench-audit.py [<v3_out_dir>] [<v2_refs_dir>]

Defaults (matches the dahouet bldgonly audit setup):
  v3_out_dir = /tmp/v3-bldgonly-audit
  v2_refs_dir = /tmp/v7-refs-bldgonly-exp
"""
import json, math, os, re, struct, sys

V3 = sys.argv[1] if len(sys.argv) > 1 else '/tmp/v3-bldgonly-audit'
V2 = sys.argv[2] if len(sys.argv) > 2 else '/tmp/v7-refs-bldgonly-exp'
TOL = 0.10  # 10 cm spatial match tolerance

# ── helpers ──────────────────────────────────────────────────────────
def load_bin(path, dt):
    with open(path,'rb') as f: b = f.read()
    if dt == 'f32': n=len(b)//4; return struct.unpack(f'{n}f', b)
    if dt == 'f64': n=len(b)//8; return struct.unpack(f'{n}d', b)
    if dt == 'i16': n=len(b)//2; return struct.unpack(f'{n}h', b)
    if dt == 'u8':                return list(b)
    raise ValueError(dt)

def load_json(path):
    with open(path) as f: return json.load(f)

def diff_bin(label, v3p, v2p, dt='f32'):
    if not os.path.exists(v3p): return f"  {label:38s} ❌ v3 MISSING"
    if not os.path.exists(v2p): return f"  {label:38s} ❌ v2 MISSING"
    A = load_bin(v3p, dt); B = load_bin(v2p, dt)
    if len(A) != len(B):
        return f"  {label:38s} ❌ LEN v3={len(A)} v2={len(B)}"
    # NaN-aware: both-NaN positions count as equal
    diffs = []
    for i, (a, b) in enumerate(zip(A, B)):
        if isinstance(a, float) and isinstance(b, float):
            if math.isnan(a) and math.isnan(b): continue
        if a != b: diffs.append(a - b if not (isinstance(a, float) and (math.isnan(a) or math.isnan(b))) else float('nan'))
    if not diffs:
        return f"  {label:38s} ✅ BYTE-EQ ({len(A)} {dt})"
    finite = [d for d in diffs if isinstance(d, float) and not math.isnan(d) or isinstance(d, int)]
    nan_only = len(diffs) - len(finite)
    if not finite:
        return f"  {label:38s} {len(diffs):>6} differ (all NaN-asymmetric)"
    m = max(abs(d) for d in finite)
    extra = f", {nan_only} NaN-asym" if nan_only else ""
    return f"  {label:38s} {len(diffs):>6}/{len(A):>6} ({100*len(diffs)/len(A):4.1f}%) max|Δ|={m:.3e}{extra}"

def pt(c):
    p = c.get('point') if isinstance(c.get('point'), dict) else c
    return (p['x'], p['y'], p['z'])

def dist(a, b):
    return ((a[0]-b[0])**2 + (a[1]-b[1])**2 + (a[2]-b[2])**2) ** 0.5

def spatial_match(v3, v2, tol=TOL):
    """Greedy nearest-neighbor match. Returns (pairs, v3_orphans, v2_orphans)."""
    v2_used = [False]*len(v2); pairs = []; v3_orph = []
    for i, c3 in enumerate(v3):
        p3 = pt(c3); best_j, best_d = -1, tol
        for j, c2 in enumerate(v2):
            if v2_used[j]: continue
            d = dist(p3, pt(c2))
            if d < best_d: best_d, best_j = d, j
        if best_j >= 0:
            pairs.append((i, best_j, best_d)); v2_used[best_j] = True
        else:
            v3_orph.append(i)
    v2_orph = [j for j, u in enumerate(v2_used) if not u]
    return pairs, v3_orph, v2_orph

# Normalize source label: v2 "candidate-concave" / "candidate-convex" → v3 "concave" / "convex"
def norm_source(s):
    if not s: return s
    if s.startswith('candidate-'): return s[len('candidate-'):]
    return s

# Coplanar-aware planeIds match.
# v3 names like "P0C13+P10CP1" mark a corner that v3 super-merged with a coplanar import
# into plane 10. v3 adds 10 to planeIds; v2 doesn't (legacy quirk). Treat as match
# when v3's extra planeIds are exactly the planes appearing as P{p}CP* in the v3 name.
CP_RE = re.compile(r'P(\d+)CP\d+')
def cp_planes(name):
    if not name: return set()
    return {int(m.group(1)) for m in CP_RE.finditer(name)}

def planeids_compare(c3, c2):
    """Return one of: 'match' | 'v3_super_cp' | 'v3_super_other' | 'v2_super' | 'asym'."""
    p3 = set(c3.get('planeIds', []))
    p2 = set(c2.get('planeIds', []))
    if p3 == p2: return 'match'
    if p3 > p2:
        extra = p3 - p2
        cpx = cp_planes(c3.get('name', ''))
        if extra <= cpx: return 'v3_super_cp'    # all extras explained by coplanar imports
        return 'v3_super_other'                   # some extras NOT from coplanar — genuine
    if p2 > p3: return 'v2_super'
    return 'asym'

# ── tile-signals (per-point .bin) ────────────────────────────────────
def audit_tile_signals():
    print("\n========== v7-tile-signals ==========")
    for f in ['point-normals','eigenvalue-l1','eigenvalue-l2','eigenvalue-l3',
              'planarity','linearity','scattering',
              'curvatures-k8','curvatures-k16','curvatures-k32',
              'eigen-reliability','point-weights']:
        print(diff_bin(f, f'{V3}/tile-signals/tile-signals_0.{f}.bin',
                       f'{V2}/tile-signals_0.{f}.bin', 'f32'))
    print(diff_bin('eigen-ternary', f'{V3}/tile-signals/tile-signals_0.eigen-ternary.bin',
                   f'{V2}/tile-signals_0.eigen-ternary.bin', 'u8'))

# ── hemisphere-segment ──────────────────────────────────────────────
def audit_hemisphere():
    print("\n========== v7-hemisphere-segment ==========")
    # cascade-segments.bin is Int16 per the v2 step (not f32) — compare as bytes
    v3p = f'{V3}/hemisphere-segment/cascade-segments.bin'
    v2p = f'{V2}/hemisphere-normals-density-aware-segment_0.cascade-segments.bin'
    if os.path.exists(v3p) and os.path.exists(v2p):
        with open(v3p,'rb') as f: a=f.read()
        with open(v2p,'rb') as f: b=f.read()
        if a == b:
            print(f"  {'cascade-segments (bytes)':38s} ✅ BYTE-EQ ({len(a)} bytes)")
        else:
            print(f"  {'cascade-segments (bytes)':38s} ❌ DIFFER (v3={len(a)} v2={len(b)})")
    print(diff_bin('ridge-candidates', f'{V3}/hemisphere-segment/ridge-candidates.bin',
                   f'{V2}/hemisphere-normals-density-aware-segment_0.ridge-candidates.bin', 'u8'))

# ── normal-cascade-seed ─────────────────────────────────────────────
def audit_cascade_seed():
    print("\n========== v7-normal-cascade-seed ==========")
    print(diff_bin('owner', f'{V3}/normal-cascade-seed/owner.bin',
                   f'{V2}/normal-cascade-seed_0.owner.bin', 'i16'))
    v3 = load_json(f'{V3}/normal-cascade-seed/seed-planes.json')
    v2 = load_json(f'{V2}/normal-cascade-seed_0.plane-equations.json')
    print(f"  {'seed-planes count':38s} v3={len(v3)} v2={len(v2)} {'✅' if len(v3)==len(v2) else '❌'}")
    if len(v3) == len(v2):
        nd=0; mn=0; md=0
        for p3, p2 in zip(v3, v2):
            n3,n2 = p3['normal'], p2['normal']
            dn = max(abs(n3[k]-n2[k]) for k in 'xyz'); dd = abs(p3['d']-p2['d'])
            if dn>0 or dd>0: nd+=1; mn=max(mn,dn); md=max(md,dd)
        print(f"  {'seed-planes per-plane':38s} {nd:>3}/{len(v3)} differ; max|Δn|={mn:.3e} max|Δd|={md:.3e}")

# ── density-voids-2 ─────────────────────────────────────────────────
def audit_density_voids():
    print("\n========== v7-density-voids-2 ==========")
    v3 = load_json(f'{V3}/density-voids-2/ghost-planes-2.json')
    v2 = load_json(f'{V2}/density-voids-2_0.ghost-planes-2.json')
    print(f"  {'ghost-planes-2 count':38s} v3={len(v3)} v2={len(v2)} {'✅' if len(v3)==len(v2) else '❌'}")
    if len(v3) == len(v2):
        nd=0; mn=0; md=0
        for p3, p2 in zip(v3, v2):
            n3 = p3.get('normal', {}); n2 = p2.get('normal', {})
            dn = max(abs(n3.get(k,0)-n2.get(k,0)) for k in 'xyz')
            dd = abs(p3.get('d',0)-p2.get('d',0))
            if dn>0 or dd>0: nd+=1; mn=max(mn,dn); md=max(md,dd)
        print(f"  {'ghost-planes per-plane':38s} {nd:>3}/{len(v3)} differ; max|Δn|={mn:.3e} max|Δd|={md:.3e}")

# ── grow-refit-merge ────────────────────────────────────────────────
def audit_grm():
    print("\n========== v7-grow-refit-merge ==========")
    print(diff_bin('owner', f'{V3}/grow-refit-merge/owner.bin',
                   f'{V2}/grow-refit-merge_0.owner.bin', 'i16'))
    v3 = load_json(f'{V3}/grow-refit-merge/plane-equations.json')
    v2 = load_json(f'{V2}/grow-refit-merge_0.plane-equations.json')
    print(f"  {'plane-equations count':38s} v3={len(v3)} v2={len(v2)} {'✅' if len(v3)==len(v2) else '❌'}")
    if len(v3) == len(v2):
        nd=0; mn=0; md=0
        for p3, p2 in zip(v3, v2):
            n3,n2 = p3['normal'], p2['normal']
            dn = max(abs(n3[k]-n2[k]) for k in 'xyz'); dd = abs(p3['d']-p2['d'])
            if dn>0 or dd>0: nd+=1; mn=max(mn,dn); md=max(md,dd)
        print(f"  {'plane-equations per-plane':38s} {nd:>3}/{len(v3)} differ; max|Δn|={mn:.3e} max|Δd|={md:.3e}")
    print(diff_bin('point-plane-residual', f'{V3}/grow-refit-merge/point-plane-residual.bin',
                   f'{V2}/grow-refit-merge_0.point-plane-residual.bin', 'f32'))

# ── beam-incidence ──────────────────────────────────────────────────
def audit_beam():
    print("\n========== v7-beam-incidence ==========")
    print(diff_bin('beam-directions', f'{V3}/beam-incidence/beam-directions.bin',
                   f'{V2}/beam-incidence_0.beam-directions.bin', 'f32'))

# ── corner-set step (boundary-signals / boundary-corners) ───────────
def audit_corners(label, v3_path, v2_path):
    print(f"\n========== {label} ==========")
    v3 = load_json(v3_path)
    if isinstance(v3, dict) and 'corners' in v3: v3 = v3['corners']
    v2 = load_json(v2_path)
    if isinstance(v2, dict) and 'corners' in v2: v2 = v2['corners']

    pairs, v3o, v2o = spatial_match(v3, v2, TOL)
    nm = len(pairs)
    print(f"  count                                  v3={len(v3)} v2={len(v2)} matched={nm} (≤{TOL}m)  v3-orph={len(v3o)}  v2-orph={len(v2o)}")
    if not nm: return

    # position diff
    byte_eq = sum(1 for _,_,d in pairs if d == 0)
    nonzero = sorted(d for _,_,d in pairs if d > 0)
    print(f"  position byte-EQ                       {byte_eq}/{nm}")
    if nonzero:
        print(f"  position diff among matched            max={nonzero[-1]:.3e}m  median={nonzero[len(nonzero)//2]:.3e}m  mean={sum(nonzero)/len(nonzero):.3e}m")

    # source label match (normalized)
    src_match = sum(1 for i,j,_ in pairs
                    if norm_source(v3[i].get('source')) == norm_source(v2[j].get('source')))
    print(f"  source label match (normalized)        {src_match}/{nm}")

    # planeIds: count categories
    cats = {'match':0, 'v3_super_cp':0, 'v3_super_other':0, 'v2_super':0, 'asym':0}
    for i, j, _ in pairs:
        cats[planeids_compare(v3[i], v2[j])] += 1
    sem_match = cats['match'] + cats['v3_super_cp']
    print(f"  planeIds match: full                   {cats['match']}/{nm}")
    print(f"  planeIds match: v3-super-by-coplanar   {cats['v3_super_cp']}/{nm}  (intentional v3 design)")
    print(f"  planeIds semantic match (full+CP)      {sem_match}/{nm} = {100*sem_match/nm:.1f}%")
    print(f"  planeIds genuine divergence            {nm-sem_match}/{nm} = {100*(nm-sem_match)/nm:.1f}%  (v3⊃v2-other:{cats['v3_super_other']} v2⊃v3:{cats['v2_super']} asym:{cats['asym']})")

# ── polygon-set step (boundary-polygons / boundary-merge) ───────────
def _resolve_v3_point(c):
    """v3 corners use point: {x,y,z} OR point: [x,y,z] depending on step. Handle both."""
    p = c.get('point')
    if p is None: return None
    if isinstance(p, dict): return (p.get('x', 0), p.get('y', 0), p.get('z', 0))
    if isinstance(p, (list, tuple)) and len(p) >= 3: return (p[0], p[1], p[2])
    return None

def audit_polygons(label, v3_dir, v2_rg_path, v3_corners_dir=None):
    print(f"\n========== {label} ==========")
    v3_polys = load_json(f'{v3_dir}/polygons.json')
    v3_edges = load_json(f'{v3_dir}/edges.json')
    # v3 polygons store cornerIds → look up positions in this step's corners.json,
    # or fall back to v3_corners_dir's corners.json (boundary-polygons doesn't dump corners,
    # so it uses boundary-corners' output as input).
    v3_corners_by_id = {}
    for cdir in (v3_dir, v3_corners_dir):
        if cdir is None: continue
        p = f'{cdir}/corners.json'
        if os.path.exists(p):
            for c in load_json(p):
                cid = c.get('id', c.get('corner_id'))
                if cid is not None: v3_corners_by_id[cid] = c
            break

    v2_rg = load_json(v2_rg_path)
    v2_edges = v2_rg.get('edges', [])
    v2_corners_by_id = {c['id']: c for c in v2_rg.get('corners', [])}
    v2_planes_wp = [p for p in v2_rg.get('planes',[]) if p.get('polygonPath') and len(p['polygonPath'])>0]
    print(f"  polygons / planes-with-polys           v3={len(v3_polys)} v2={len(v2_planes_wp)} {'✅' if len(v3_polys)==len(v2_planes_wp) else '❌'}")
    print(f"  edges                                  v3={len(v3_edges)} v2={len(v2_edges)} {'✅' if len(v3_edges)==len(v2_edges) else '❌'}")
    # shared edges
    v3_sh = sum(1 for e in v3_edges if isinstance(e.get('plane_ids', e.get('planeIds')), list) and len(e.get('plane_ids', e.get('planeIds')))>=2)
    v2_sh = sum(1 for e in v2_edges if isinstance(e.get('planeIds'), list) and len(e['planeIds'])>=2)
    print(f"  shared edges                           v3={v3_sh} v2={v2_sh} {'✅' if v3_sh==v2_sh else '❌'}")

    # polygons by plane_id
    v3_by_plane = {p.get('plane_id', p.get('planeId')): p for p in v3_polys}
    v2_by_plane = {p['id']: p for p in v2_planes_wp}
    common = set(v3_by_plane) & set(v2_by_plane)
    print(f"  polygons by plane                      common={len(common)} v3-only={len(set(v3_by_plane)-set(v2_by_plane))} v2-only={len(set(v2_by_plane)-set(v3_by_plane))}")
    if not common: return

    # Resolve v3 polygon to positions via cornerIds.
    # Compare polygons via spatial set-match (cycle rotation/reflection insensitive):
    #   for each v3 polygon, get sorted set of vertex positions; same for v2.
    same_vc = 0; better_vc = 0
    matched_unordered = 0; matched_ordered = 0
    pos_diffs = []
    samples = []
    for pid in sorted(common):
        p3 = v3_by_plane[pid]; p2 = v2_by_plane[pid]
        v3_cids = p3.get('cornerIds', [])
        v3_path = []
        for cid in v3_cids:
            c = v3_corners_by_id.get(cid)
            if c:
                pt = _resolve_v3_point(c)
                if pt: v3_path.append(pt)
        v2_cids = p2.get('polygonPath', [])
        v2_path = []
        for cid in v2_cids:
            c = v2_corners_by_id.get(cid)
            if c and 'point' in c:
                pt = c['point']
                v2_path.append((pt['x'], pt['y'], pt['z']))
        if len(v3_path) == len(v2_path):
            same_vc += 1
            # Unordered spatial match: greedy nearest within 10 cm
            v2_used = [False]*len(v2_path); all_matched = True
            for a in v3_path:
                best_j, best_d = -1, TOL
                for j, b in enumerate(v2_path):
                    if v2_used[j]: continue
                    d = ((a[0]-b[0])**2+(a[1]-b[1])**2+(a[2]-b[2])**2)**0.5
                    if d < best_d: best_d, best_j = d, j
                if best_j < 0: all_matched = False; break
                v2_used[best_j] = True
                pos_diffs.append(best_d)
            if all_matched:
                matched_unordered += 1
                # Ordered (cycle-rotation-insensitive): find offset where they align
                n = len(v3_path); ordered = False
                for off in range(n):
                    ok = True
                    for k in range(n):
                        a = v3_path[k]; b = v2_path[(k+off) % n]
                        d = ((a[0]-b[0])**2+(a[1]-b[1])**2+(a[2]-b[2])**2)**0.5
                        if d > TOL: ok = False; break
                    if ok: ordered = True; break
                if ordered: matched_ordered += 1
        else:
            if len(samples) < 5:
                samples.append((pid, len(v3_path), len(v2_path)))
    print(f"  polygons with matching vertex count    {same_vc}/{len(common)}")
    print(f"  polygons w/ matching vertices (set)    {matched_unordered}/{len(common)} (cycle-rotation insensitive)")
    print(f"  polygons w/ matching vertices (ordered){matched_ordered}/{len(common)} (cycle-rotation aware)")
    if pos_diffs:
        pos_diffs.sort()
        print(f"  vertex pos diff (matched vertices)     max={pos_diffs[-1]:.3e}m  median={pos_diffs[len(pos_diffs)//2]:.3e}m")
    if samples:
        print(f"  vertex-count mismatch samples (plane_id, v3, v2): {samples}")

# ── main ─────────────────────────────────────────────────────────────
def main():
    print(f"v3 out:  {V3}")
    print(f"v2 refs: {V2}")
    print(f"spatial-match tolerance: {TOL} m")
    audit_tile_signals()
    audit_hemisphere()
    audit_cascade_seed()
    audit_density_voids()
    audit_grm()
    audit_beam()
    audit_corners('v7-boundary-signals (corners)',
                  f'{V3}/boundary-signals/corners.json',
                  f'{V2}/boundary-signals_0.roof-graph.json')
    audit_corners('v7-boundary-corners (corners)',
                  f'{V3}/boundary-corners/corners.json',
                  f'{V2}/boundary-corners_0.roof-graph.json')
    audit_polygons('v7-boundary-polygons',
                   f'{V3}/boundary-polygons',
                   f'{V2}/boundary-polygons_0.roof-graph.json',
                   v3_corners_dir=f'{V3}/boundary-corners')
    audit_polygons('v7-boundary-merge',
                   f'{V3}/boundary-merge',
                   f'{V2}/boundary-merge_0.roof-graph.json')

if __name__ == '__main__': main()
