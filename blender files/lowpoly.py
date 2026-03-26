import sys
import numpy as np
from PIL import Image, ImageDraw
from scipy.spatial import Delaunay

def lowpoly(input_path, output_path, num_points=800):
    img = Image.open(input_path).convert('RGB')
    w, h = img.size
    arr = np.array(img)
    print(f"Image: {w}x{h}, points: {num_points}")

    # Generate edge-aware sample points using gradient magnitude
    from PIL import ImageFilter
    gray = img.convert('L')
    edges = gray.filter(ImageFilter.FIND_EDGES)
    edge_arr = np.array(edges, dtype=np.float32)

    # Normalize edge map as probability
    edge_arr += 5.0  # base probability even in flat areas
    edge_arr /= edge_arr.sum()

    # Sample points weighted by edges
    flat = edge_arr.flatten()
    indices = np.random.choice(len(flat), size=num_points, replace=False, p=flat)
    ys, xs = np.unravel_index(indices, (h, w))

    # Add corner and border points so triangles cover full image
    border = []
    for bx in np.linspace(0, w-1, 12, dtype=int):
        border += [(bx, 0), (bx, h-1)]
    for by in np.linspace(0, h-1, 12, dtype=int):
        border += [(0, by), (w-1, by)]
    bxs = [p[0] for p in border]
    bys = [p[1] for p in border]

    all_x = np.concatenate([xs, bxs])
    all_y = np.concatenate([ys, bys])
    points = np.stack([all_x, all_y], axis=1).astype(np.float32)

    # Delaunay triangulation
    tri = Delaunay(points)
    print(f"Triangles: {len(tri.simplices)}")

    # Draw flat-shaded triangles
    result = Image.new('RGB', (w, h), (0,0,0))
    draw = ImageDraw.Draw(result)

    for simplex in tri.simplices:
        pts = points[simplex]  # 3 x 2
        x0,y0 = pts[0]; x1,y1 = pts[1]; x2,y2 = pts[2]

        # Sample color at centroid
        cx = int(np.clip((x0+x1+x2)/3, 0, w-1))
        cy = int(np.clip((y0+y1+y2)/3, 0, h-1))

        # Average color over a small patch around centroid
        px0 = max(0, cx-3); px1 = min(w, cx+4)
        py0 = max(0, cy-3); py1 = min(h, cy+4)
        patch = arr[py0:py1, px0:px1]
        r,g,b = int(patch[:,:,0].mean()), int(patch[:,:,1].mean()), int(patch[:,:,2].mean())

        poly = [(int(x0),int(y0)), (int(x1),int(y1)), (int(x2),int(y2))]
        draw.polygon(poly, fill=(r,g,b))
        # Sharp edge lines for the faceted look
        draw.line(poly + [poly[0]], fill=(r-15,g-15,b-15), width=1)

    result.save(output_path)
    print(f"Saved: {output_path}")

if __name__ == '__main__':
    inp  = sys.argv[1] if len(sys.argv) > 1 else 'face.png'
    out  = sys.argv[2] if len(sys.argv) > 2 else 'face_lowpoly.png'
    pts  = int(sys.argv[3]) if len(sys.argv) > 3 else 800
    lowpoly(inp, out, pts)
    print("Done! Try fewer points (400) for more angular, more (1500) for finer detail.")
