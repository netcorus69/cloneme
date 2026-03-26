# export_mask.py
import bpy
import bmesh
import os

# === CONFIGURE ===
OUT_PATH = "/home/netcorus/workfile/progetti/miei progetti/cloneme/blender files/monkey.mask"
GROUP_NAME = "Mouth"   # change to the vertex group you painted
OBJ_OBJECT_NAME = None # optional: set to object name if multiple objects present
# =================

def ensure_triangulated(obj):
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces[:], quad_method='BEAUTY', ngon_method='BEAUTY')
    bm.free()

def write_mask_for_object(obj, out_path, group_name):
    deps = bpy.context.evaluated_depsgraph_get()
    eval_obj = obj.evaluated_get(deps)
    eval_mesh = eval_obj.data

    vcount = len(eval_mesh.vertices)
    print(f"Object '{obj.name}' evaluated vertex count: {vcount}")

    orig_mesh = obj.data
    if group_name not in [g.name for g in obj.vertex_groups]:
        print(f"Warning: vertex group '{group_name}' not found on object '{obj.name}'. All weights will be 0.0")

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        for vi in range(len(orig_mesh.vertices)):
            w = 0.0
            for g in orig_mesh.vertices[vi].groups:
                try:
                    if obj.vertex_groups[g.group].name == group_name:
                        w = g.weight
                        break
                except Exception:
                    pass
            f.write("{:.6f}\n".format(w))
    print(f"Wrote mask to: {out_path}")

def main():
    obj = None
    if OBJ_OBJECT_NAME:
        obj = bpy.data.objects.get(OBJ_OBJECT_NAME)
        if not obj:
            print(f"Object named '{OBJ_OBJECT_NAME}' not found.")
            return
    else:
        obj = bpy.context.view_layer.objects.active
        if not obj:
            for o in bpy.data.objects:
                if o.type == 'MESH':
                    obj = o
                    break
    if not obj or obj.type != 'MESH':
        print("No mesh object selected or found. Select your head mesh and run again.")
        return

    print(f"Using object: {obj.name}")
    ensure_triangulated(obj)
    write_mask_for_object(obj, OUT_PATH, GROUP_NAME)

if __name__ == "__main__":
    main()
