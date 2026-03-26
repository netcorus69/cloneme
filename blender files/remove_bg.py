import sys
import numpy as np
from PIL import Image

def remove_background(input_path, output_path, tolerance=30):
    img = Image.open(input_path).convert('RGBA')
    arr = np.array(img, dtype=np.float32)

    # Sample background color from the 4 corners (5x5 average each)
    corners = [
        arr[:5,   :5,   :3],
        arr[:5,   -5:,  :3],
        arr[-5:,  :5,   :3],
        arr[-5:,  -5:,  :3],
    ]
    bg_color = np.mean([c.mean(axis=(0,1)) for c in corners], axis=0)
    print(f"Detected background color: R={bg_color[0]:.0f} G={bg_color[1]:.0f} B={bg_color[2]:.0f}")

    # Calculate distance of each pixel from background color
    rgb = arr[:, :, :3]
    dist = np.sqrt(np.sum((rgb - bg_color) ** 2, axis=2))

    # Create alpha mask: bg pixels become transparent
    alpha = np.where(dist < tolerance, 0, 255).astype(np.uint8)

    # Smooth edges slightly
    from PIL import ImageFilter
    alpha_img = Image.fromarray(alpha).filter(ImageFilter.GaussianBlur(radius=1))
    alpha_arr = np.array(alpha_img)

    result = arr.copy().astype(np.uint8)
    result[:, :, 3] = alpha_arr

    Image.fromarray(result, 'RGBA').save(output_path)
    print(f"Saved: {output_path}")
    removed = np.sum(alpha_arr < 128)
    print(f"Removed ~{removed} background pixels")

if __name__ == '__main__':
    input_file  = sys.argv[1] if len(sys.argv) > 1 else 'face.png'
    output_file = sys.argv[2] if len(sys.argv) > 2 else 'face_nobg.png'
    tolerance   = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    remove_background(input_file, output_file, tolerance)
    print("Done! If edges look rough, try a higher tolerance: python remove_bg.py face.png face_nobg.png 50")
