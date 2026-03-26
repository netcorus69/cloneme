import sys
from PIL import Image, ImageChops
import numpy as np

def bake(uv_path, face_path, output_path, preview_path):
    uv_img  = Image.open(uv_path).convert('RGBA')
    face_img = Image.open(face_path).convert('RGBA')

    uw, uh = uv_img.size
    print(f"UV layout size: {uw}x{uh}")

    # Resize face to match UV canvas
    face_resized = face_img.resize((uw, uh), Image.LANCZOS)

    # Save as the actual texture to use in renderer
    face_resized.save(output_path)
    print(f"Texture saved: {output_path}")

    # Make UV overlay semi-transparent green for preview
    uv_arr = np.array(uv_img.convert('L'))  # greyscale
    # Where UV lines are dark (mesh lines), make them visible overlay
    overlay = np.zeros((uh, uw, 4), dtype=np.uint8)
    mask = uv_arr < 128  # dark pixels = UV edges
    overlay[mask] = [0, 255, 0, 180]  # green lines

    uv_overlay = Image.fromarray(overlay, 'RGBA')

    # Composite: face + green UV lines on top
    preview = face_resized.copy()
    preview = Image.alpha_composite(preview, uv_overlay)
    preview.save(preview_path)
    print(f"Preview saved: {preview_path}  (shows UV lines on face)")

if __name__ == '__main__':
    uv_path      = sys.argv[1] if len(sys.argv) > 1 else 'head_fixed.png'
    face_path    = sys.argv[2] if len(sys.argv) > 2 else 'face.png'
    output_path  = sys.argv[3] if len(sys.argv) > 3 else 'face_baked.png'
    preview_path = sys.argv[4] if len(sys.argv) > 4 else 'face_preview.png'
    bake(uv_path, face_path, output_path, preview_path)
