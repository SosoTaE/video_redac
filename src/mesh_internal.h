#ifndef VIDEO_REDAC_MESH_INTERNAL_H
#define VIDEO_REDAC_MESH_INTERNAL_H

/*
 * mesh_internal.h — shared between mesh.c and the individual format readers.
 *
 * Not part of the public interface: everything outside src/ goes through
 * mesh_load() in mesh.h and never sees how a file becomes triangles. This
 * header exists only so a reader can append geometry using the same growth
 * helpers mesh.c uses, rather than each format inventing its own buffers and
 * then copying them across.
 */

#include "types.h"

/* Appends one vertex; `cap` is the caller's capacity counter for m->verts. */
bool vr_mesh_push_vert(MeshWidget *m, size_t *cap, float x, float y, float z);

/* Appends one triangle. Degenerate index triples are silently dropped. */
bool vr_mesh_push_tri(MeshWidget *m, size_t *cap, int a, int b, int c);

/* Grows the normal and UV arrays to `cap` vertices. Call before set_attrib. */
bool vr_mesh_ensure_attribs(MeshWidget *m, size_t cap);

/* Records the normal and UV of the vertex most recently pushed. */
void vr_mesh_set_attrib(MeshWidget *m, float nx, float ny, float nz,
                        float u, float v);

/*
 * Reads a glTF 2.0 file — either .gltf (JSON, with external or base64 buffers)
 * or .glb (the binary container) — into `m`.
 *
 * Geometry only: positions, normals, texture coordinates, triangle indices and
 * the node transforms that place them. Animation, skinning and the PBR material
 * model are deliberately out of scope; the one material property read is the
 * base colour texture, because without it a textured model arrives blank.
 */
bool vr_mesh_load_gltf(MeshWidget *m, size_t *vc, size_t *tc);

#endif /* VIDEO_REDAC_MESH_INTERNAL_H */
