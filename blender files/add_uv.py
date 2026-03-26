import sys
import os
import math

def add_uv_to_obj(input_path, output_path, u_offset=0.0, v_offset=0.0):
    vertices = []
    other_lines = []

    print(f"Reading: {input_path}")

    with open(input_path, 'r') as f:
        lines = f.readlines()

    for line in lines:
        parts = line.strip().split()
        if not parts:
            other_lines.append(('empty', line))
            continue
        if parts[0] == 'v':
            x, y, z = float(parts[1]), float(parts[2]), float(parts[3])
            vertices.append((x, y, z))
            other_lines.append(('v', line))
        elif parts[0] == 'f':
            other_lines.append(('f', parts[1:]))
        else:
            other_lines.append(('other', line))

    print(f"Found {len(vertices)} vertices")

    cx = sum(v[0] for v in vertices) / len(vertices)
    cy = sum(v[1] for v in vertices) / len(vertices)
    cz = sum(v[2] for v in vertices) / len(vertices)
    print(f"Mesh center: ({cx:.3f}, {cy:.3f}, {cz:.3f})")

    uvs = []
    for (x, y, z) in vertices:
        dx = x - cx
        dy = y - cy
        dz = z - cz

        length = math.sqrt(dx*dx + dy*dy + dz*dz)
        if length == 0:
            uvs.append((0.5, 0.5))
            continue
        dx /= length
        dy /= length
        dz /= length

        # face points +Y: atan2(dx, dy) centers face at U=0.5
        u = (0.5 + u_offset + math.atan2(dx, dy) / (2 * math.pi)) % 1.0
        v = (0.5 + v_offset + math.asin(max(-1.0, min(1.0, dz))) / math.pi) % 1.0

        uvs.append((u, v))

    print(f"Generated {len(uvs)} UV coordinates")

    with open(output_path, 'w') as f:
        for (kind, data) in other_lines:
            if kind in ('empty', 'v', 'other'):
                f.write(data)

        f.write('\n# UV Coordinates (spherical)\n')
        for (u, v) in uvs:
            f.write(f'vt {u:.6f} {v:.6f}\n')

        f.write('\n')
        for (kind, data) in other_lines:
            if kind == 'f':
                new_face = 'f'
                for token in data:
                    sub = token.split('/')
                    vi = int(sub[0])
                    vti = vi
                    if len(sub) == 1:
                        new_face += f' {vi}/{vti}'
                    elif len(sub) == 2:
                        new_face += f' {vi}/{vti}'
                    elif len(sub) == 3:
                        vni = sub[2] if sub[2] else ''
                        new_face += f' {vi}/{vti}/{vni}' if vni else f' {vi}/{vti}/'
                f.write(new_face + '\n')

    print(f"Done! Saved to: {output_path}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python add_uv.py input.obj [output.obj] [u_offset]")
        sys.exit(1)
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) >= 3 else os.path.splitext(input_file)[0] + '_uv.obj'
    u_offset = float(sys.argv[3]) if len(sys.argv) >= 4 else 0.0
    v_offset = float(sys.argv[4]) if len(sys.argv) >= 5 else 0.0
    add_uv_to_obj(input_file, output_file, u_offset, v_offset)
