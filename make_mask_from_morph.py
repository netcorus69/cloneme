#!/usr/bin/env python3
"""
make_mask_from_morph.py

Usage:
  python make_mask_from_morph.py base.obj morph.obj out.mask [--low L] [--high H] [--invert] [--binary]

Description:
  Compares vertex positions in base.obj and morph.obj (same vertex order).
  Computes per-vertex distance d = ||v_morph - v_base|| and maps d to weight w in [0,1]
  using smoothstep between low and high thresholds:
      w = smoothstep(low, high, d)
  Options:
    --low L     : lower threshold (default 0.0005)
    --high H    : upper threshold (default 0.005)
    --invert    : invert weights (1 - w)
    --binary    : output 0 or 1 using high as cutoff (w = d >= high ? 1 : 0)
"""
import sys, math, argparse

def read_vertices(obj_path):
    verts = []
    with open(obj_path, 'r', encoding='utf8', errors='ignore') as f:
        for line in f:
            if line.startswith('v '):
                parts = line.strip().split()
                if len(parts) < 4: continue
                x,y,z = float(parts[1]), float(parts[2]), float(parts[3])
                verts.append((x,y,z))
    return verts

def vec_sub(a,b):
    return (a[0]-b[0], a[1]-b[1], a[2]-b[2])

def length(v):
    return math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])

def smoothstep(edge0, edge1, x):
    if x <= edge0: return 0.0
    if x >= edge1: return 1.0
    t = (x - edge0) / (edge1 - edge0)
    return t * t * (3.0 - 2.0 * t)

def main():
    p = argparse.ArgumentParser(description="Create mask from base and morph OBJ")
    p.add_argument('base', help='base OBJ path')
    p.add_argument('morph', help='morph OBJ path')
    p.add_argument('out', help='output mask path')
    p.add_argument('--low', type=float, default=0.0005, help='smoothstep low threshold (default 0.0005)')
    p.add_argument('--high', type=float, default=0.005, help='smoothstep high threshold (default 0.005)')
    p.add_argument('--invert', action='store_true', help='invert weights')
    p.add_argument('--binary', action='store_true', help='output binary mask using high as cutoff')
    args = p.parse_args()

    base_verts = read_vertices(args.base)
    morph_verts = read_vertices(args.morph)

    if len(base_verts) == 0:
        print("ERROR: no vertices found in base OBJ:", args.base); sys.exit(1)
    if len(base_verts) != len(morph_verts):
        print("ERROR: vertex count mismatch")
        print(" base:", len(base_verts), "morph:", len(morph_verts))
        sys.exit(1)

    with open(args.out, 'w') as out_f:
        for i,(vb,vm) in enumerate(zip(base_verts, morph_verts)):
            d = length(vec_sub(vm, vb))
            if args.binary:
                w = 1.0 if d >= args.high else 0.0
            else:
                w = smoothstep(args.low, args.high, d)
            if args.invert:
                w = 1.0 - w
            out_f.write("{:.6f}\n".format(w))
    print("Wrote mask:", args.out, "vertices:", len(base_verts))

if __name__ == '__main__':
    main()
