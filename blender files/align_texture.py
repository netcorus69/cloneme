#!/usr/bin/env python3
# align_texture.py
# Automatically places face.png onto the UV layout (head_fixed.png)
# Run: python3 align_texture.py
# Output: face_uv.png  → rename this to face.png

from PIL import Image

UV_MAP   = "face.png"    # UV layout from Blender
FACE_IMG = "facey.png"   # real face photo
OUTPUT   = "face_out.png"

# Load UV layout to get canvas size
uv   = Image.open(UV_MAP).convert("RGBA")
face = Image.open(FACE_IMG).convert("RGBA")

W, H = uv.size   # canvas size (likely 1024x1024)
print(f"UV canvas size: {W}x{H}")

# Create a black background canvas
canvas = Image.new("RGBA", (W, H), (0, 0, 0, 255))

# --- TUNE THESE VALUES to move/scale the face ---
# The front face UV island is roughly at bottom-left of the UV map
# Adjust these until the face aligns correctly in 3D

FACE_SCALE  = 0.50   # slightly bigger
FACE_X      = 0.30   # much more left
FACE_Y      = 0.65   # much more down

# Compute pixel size and position
fw = int(W * FACE_SCALE)
fh = int(fw * face.height / face.width)  # keep aspect ratio
face_resized = face.resize((fw, fh), Image.LANCZOS)

px = int(W * FACE_X - fw / 2)
py = int(H * FACE_Y - fh / 2)

print(f"Placing face at ({px}, {py}), size {fw}x{fh}")

# Paste face onto canvas
canvas.paste(face_resized, (px, py))

# Convert to RGB and save
result = canvas.convert("RGB")
result.save(OUTPUT)
print(f"Saved: {OUTPUT}")
print()
print("If the texture looks wrong in 3D, tweak these values in the script:")
print("  FACE_SCALE  — make face bigger or smaller")
print("  FACE_X      — move face left (0.0) or right (1.0)")
print("  FACE_Y      — move face up (0.0) or down (1.0)")
print("Then re-run and copy face_uv.png → face.png")
