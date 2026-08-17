#ifndef VIDEO_REDAC_MESH_H
#define VIDEO_REDAC_MESH_H

/*
 * mesh.h — loading and generating triangle meshes.
 *
 * Two sources, one representation: a model file on disk — OBJ, or glTF 2.0 in
 * either the .gltf or .glb container — or a procedural primitive named in the
 * JSON. All end up as a vertex array, optional normals and UVs, and a triangle
 * index array, normalised into a unit cube centred on the origin.
 *
 * The normalisation matters more than it looks. Models arrive in whatever units
 * their exporter used — metres, millimetres, arbitrary — and often nowhere near
 * the origin. Without it, `"size": 300` would mean something different for
 * every file, and half of them would render off-screen.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fills `m->verts` / `m->tris` from `m->path` (a model file, dispatched on its
 * extension) or `m->shape` (a primitive). Returns false and leaves the mesh
 * empty on failure.
 */
bool mesh_load(MeshWidget *m);

/* Frees the vertex and triangle arrays. Safe on NULL and on an empty mesh. */
void mesh_free(MeshWidget *m);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_MESH_H */
