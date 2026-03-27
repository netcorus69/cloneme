# export_all_masks.py
import bpy, os

OUT_DIR = "/home/netcorus/workfile/progetti/miei progetti/cloneme/blender files/masks"
OBJ_OBJECT_NAME = None  # optional: set to object name if multiple objects present

def get_target_object():
    if OBJ_OBJECT_NAME:
        return bpy.data.objects.get(OBJ_OBJECT_NAME)
    obj = bpy.context.view_layer.objects.active
    if obj and obj.type == 'MESH':
        return obj
    for o in bpy.data.objects:
        if o.type == 'MESH':
            return o
    return None

def write_mask(obj, group_name, out_path):
    mesh = obj.data
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        for vi in range(len(mesh.vertices)):
            w = 0.0
            for g in mesh.vertices[vi].groups:
                try:
                    if obj.vertex_groups[g.group].name == group_name:
                        w = g.weight
                        break
                except Exception:
                    pass
            f.write("{:.6f}\n".format(w))
    print("Wrote:", out_path)

def main():
    obj = get_target_object()
    if not obj:
        print("No mesh object found. Select your head mesh and run again.")
        return
    if len(obj.vertex_groups) == 0:
        print("No vertex groups found on object:", obj.name)
        return
    print("Using object:", obj.name)
    for g in obj.vertex_groups:
        safe_name = g.name.replace(" ", "_")
        out_path = os.path.join(OUT_DIR, f"{safe_name}.mask")
        write_mask(obj, g.name, out_path)

if __name__ == "__main__":
    main()
