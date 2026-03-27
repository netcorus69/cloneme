#!/usr/bin/env bash
set -euo pipefail

# Paths (adjust if your repo is elsewhere)
REPO_DIR="$(pwd)"                # run this script from the repo root
BASE_OBJ="$REPO_DIR/monkey.obj"
OUT_DIR="$REPO_DIR/masks"

# Ensure mask script exists
MASK_SCRIPT="$REPO_DIR/make_mask_from_morph.py"
if [ ! -f "$MASK_SCRIPT" ]; then
  echo "Error: make_mask_from_morph.py not found in $REPO_DIR"
  exit 1
fi

# Create masks folder
mkdir -p "$OUT_DIR"
echo "Masks will be written to: $OUT_DIR"

# List of morph OBJs to process (add or remove names as needed)
MORPHS=(
  "monkey_open.obj"
  "monkey_wide.obj"
  "monkeyeyes.obj"
)

# Basic thresholds (tweak --low/--high per morph if needed)
LOW=0.0005
HIGH=0.005

# Run generator for each morph that exists
for m in "${MORPHS[@]}"; do
  MORPH_PATH="$REPO_DIR/$m"
  if [ -f "$MORPH_PATH" ]; then
    OUT_MASK="$OUT_DIR/$(basename "$m" .obj).mask"
    echo "Generating mask for $m -> $(basename "$OUT_MASK")"
    python3 "$MASK_SCRIPT" "$BASE_OBJ" "$MORPH_PATH" "$OUT_MASK" --low "$LOW" --high "$HIGH"
  else
    echo "Skipping $m (file not found)"
  fi
done

# Quick verification
echo
echo "Verification:"
VERTS=$(grep -c '^v ' "$BASE_OBJ" || true)
echo "Base OBJ vertices: $VERTS"
for f in "$OUT_DIR"/*.mask; do
  [ -f "$f" ] || continue
  LINES=$(wc -l < "$f")
  NAME=$(basename "$f")
  if [ "$LINES" -eq "$VERTS" ]; then
    echo " OK: $NAME ($LINES lines)"
  else
    echo " MISMATCH: $NAME ($LINES lines) != $VERTS vertices"
  fi
done

echo
echo "Done. Masks saved in: $OUT_DIR"
