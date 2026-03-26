import sys
import numpy as np
from PIL import Image, ImageFilter

def replace_bg_with_skin(input_path, output_path, tolerance=40):
    img = Image.open(input_path).convert('RGB')
    arr = np.array(img, dtype=np.float32)

    # Sample background from corners
    corners = [arr[:10,:10,:3], arr[:10,-10:,:3], arr[-10:,:10,:3], arr[-10:,-10:,:3]]
    bg_color = np.mean([c.mean(axis=(0,1)) for c in corners], axis=0)
    print(f"Background color: R={bg_color[0]:.0f} G={bg_color[1]:.0f} B={bg_color[2]:.0f}")

    # Sample skin from center of image
    h, w = arr.shape[:2]
    center = arr[h//3:2*h//3, w//3:2*w//3, :3]
    skin_color = center.mean(axis=(0,1))
    print(f"Skin color: R={skin_color[0]:.0f} G={skin_color[1]:.0f} B={skin_color[2]:.0f}")

    # Find background pixels
    dist = np.sqrt(np.sum((arr[:,:,:3] - bg_color)**2, axis=2))
    bg_mask = dist < tolerance

    # Replace background with skin color
    result = arr.copy()
    result[bg_mask, 0] = skin_color[0]
    result[bg_mask, 1] = skin_color[1]
    result[bg_mask, 2] = skin_color[2]

    Image.fromarray(result.astype(np.uint8), 'RGB').save(output_path)
    print(f"Saved: {output_path}")

if __name__ == '__main__':
    input_file  = sys.argv[1] if len(sys.argv) > 1 else 'face.png'
    output_file = sys.argv[2] if len(sys.argv) > 2 else 'face_skinbg.png'
    tolerance   = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    replace_bg_with_skin(input_file, output_file, tolerance)
