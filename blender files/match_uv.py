import sys
import numpy as np
from PIL import Image

def match_to_uv(uv_path, face_path, output_path):
    uv_img  = Image.open(uv_path).convert('L')   # greyscale
    face_img = Image.open(face_path).convert('RGBA')

    uw, uh = uv_img.size
    print(f"UV layout: {uw}x{uh}")

    uv_arr = np.array(uv_img)

    # Find bounding box of the dark head shape in UV
    mask = uv_arr < 128  # dark pixels = mesh
    rows = np.any(mask, axis=1)
    cols = np.any(mask, axis=0)
    rmin, rmax = np.where(rows)[0][[0, -1]]
    cmin, cmax = np.where(cols)[0][[0, -1]]

    head_w = cmax - cmin
    head_h = rmax - rmin
    print(f"Head bounding box: x={cmin}-{cmax}, y={rmin}-{rmax} ({head_w}x{head_h})")

    # Resize face to fit the head bounding box
    face_resized = face_img.resize((head_w, head_h), Image.LANCZOS)

    # Create transparent canvas same size as UV
    canvas = Image.new('RGBA', (uw, uh), (0, 0, 0, 0))

    # Paste face into the head bounding box position
    canvas.paste(face_resized, (cmin, rmin))

    # Fill areas outside head shape with average skin color
    face_arr = np.array(face_resized)
    # Sample skin from center strip of face
    center = face_arr[head_h//3:2*head_h//3, head_w//4:3*head_w//4]
    avg_skin = center.reshape(-1,4).mean(axis=0).astype(np.uint8)
    print(f"Avg skin color: R={avg_skin[0]} G={avg_skin[1]} B={avg_skin[2]}")

    # Create full canvas filled with skin color
    skin_canvas = Image.new('RGBA', (uw, uh), (int(avg_skin[0]), int(avg_skin[1]), int(avg_skin[2]), 255))
    # Paste face on top
    skin_canvas.paste(canvas, (0, 0), canvas)

    skin_canvas.save(output_path)
    print(f"Saved: {output_path}")

if __name__ == '__main__':
    uv_path   = sys.argv[1] if len(sys.argv) > 1 else 'head_fixed.png'
    face_path = sys.argv[2] if len(sys.argv) > 2 else 'face.png'
    out_path  = sys.argv[3] if len(sys.argv) > 3 else 'face_matched.png'
    match_to_uv(uv_path, face_path, out_path)
