#!/usr/bin/env python3
# expand_mask.py
# Usage: python3 expand_mask.py path/to/model.obj path/to/orig.mask path/to/out_expanded.mask

import sys
import os

def load_mask(path):
    vals = []
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line: continue
            try:
                vals.append(float(line.split()[0]))
            except:
                # tolerate lines with extra whitespace
                vals.append(float(line))
    return vals

def expand_mask(obj_path, orig_mask_path, out_path):
    orig = load_mask(orig_mask_path)
    if len(orig) == 0:
        print("Error: original mask empty:", orig_mask_path)
        return 1

    expanded = []
    with open(obj_path, 'r') as f:
        for ln in f:
            ln = ln.strip()
            if not ln: continue
            if ln.startswith('f '):
                parts = ln.split()[1:]
                for p in parts:
                    # face element can be v, v/vt, v//vn, v/vt/vn
                    v = p.split('/')[0]
                    try:
                        vi = int(v)
                    except:
                        print("Warning: unexpected face token:", p)
                        continue
                    # OBJ indices are 1-based, support negative indices
                    if vi < 0:
                        # negative index: count from end of positions; we need total positions
                        # to support negative indices we'd need to parse all 'v' lines first.
                        # For simplicity, we will error if negative indices are present.
                        raise RuntimeError("Negative vertex indices not supported in this script.")
                    idx = vi - 1
                    if idx < 0 or idx >= len(orig):
                        # out of range: write 0.0 and warn
                        expanded.append(0.0)
                    else:
                        expanded.append(orig[idx])
    # write expanded mask
    with open(out_path, 'w') as out:
        for v in expanded:
            out.write("{:.6f}\n".format(v))
    print("Wrote expanded mask:", out_path, "entries:", len(expanded))
    return 0

def main():
    if len(sys.argv) != 4:
        print("Usage: python3 expand_mask.py model.obj orig.mask out_expanded.mask")
        return 2
    obj_path = sys.argv[1]
    orig_mask = sys.argv[2]
    out_mask = sys.argv[3]
    if not os.path.isfile(obj_path):
        print("Error: obj not found:", obj_path); return 1
    if not os.path.isfile(orig_mask):
        print("Error: orig mask not found:", orig_mask); return 1
    return expand_mask(obj_path, orig_mask, out_mask)

if __name__ == '__main__':
    sys.exit(main())
