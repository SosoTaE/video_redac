/*
 * mesh.c — OBJ loading and procedural primitives.
 *
 * Both paths produce the same thing: a flat vertex array and a triangle index
 * array, normalised into a unit cube centred on the origin. Everything
 * downstream — transform, projection, rasterization — then works on one
 * representation and never has to care where the geometry came from.
 */

#include "mesh.h"
#include "mesh_internal.h"

/* The generators below predate the split and keep the short names. */
#define push_vert           vr_mesh_push_vert
#define push_tri            vr_mesh_push_tri
#define ensure_attribs      vr_mesh_ensure_attribs
#define set_attrib          vr_mesh_set_attrib

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Growth is geometric, as everywhere else in the project. */
bool (vr_mesh_push_vert)(MeshWidget *m, size_t *cap, float x, float y, float z)
{
    if (m->vert_count == *cap) {
        size_t nc = (*cap == 0) ? 256 : *cap * 2;
        float *g = (float *)realloc(m->verts, nc * 3 * sizeof(float));
        if (g == NULL) {
            return false;
        }
        m->verts = g;
        *cap = nc;
    }
    m->verts[m->vert_count * 3 + 0] = x;
    m->verts[m->vert_count * 3 + 1] = y;
    m->verts[m->vert_count * 3 + 2] = z;
    m->vert_count++;
    return true;
}

bool (vr_mesh_push_tri)(MeshWidget *m, size_t *cap, int a, int b, int c)
{
    if (a == b || b == c || a == c) {
        return true;      /* degenerate; silently skipped */
    }
    if (m->tri_count == *cap) {
        size_t nc = (*cap == 0) ? 256 : *cap * 2;
        MeshTri *g = (MeshTri *)realloc(m->tris, nc * sizeof(MeshTri));
        if (g == NULL) {
            return false;
        }
        m->tris = g;
        *cap = nc;
    }
    m->tris[m->tri_count].v[0] = a;
    m->tris[m->tri_count].v[1] = b;
    m->tris[m->tri_count].v[2] = c;
    m->tri_count++;
    return true;
}

/*
 * Vertex attributes live in arrays parallel to `verts`, so a vertex index
 * addresses all three. Meshes that have no normals or UVs leave those NULL
 * rather than filling them with defaults nothing reads.
 */
bool (vr_mesh_ensure_attribs)(MeshWidget *m, size_t cap)
{
    float *n = (float *)realloc(m->norms, cap * 3 * sizeof(float));
    if (n == NULL) return false;
    m->norms = n;

    float *u = (float *)realloc(m->uvs, cap * 2 * sizeof(float));
    if (u == NULL) return false;
    m->uvs = u;
    return true;
}

/* Records the normal and UV for the vertex just pushed. */
void (vr_mesh_set_attrib)(MeshWidget *m, float nx, float ny, float nz, float u, float v)
{
    size_t i = m->vert_count - 1;
    m->norms[i * 3 + 0] = nx;
    m->norms[i * 3 + 1] = ny;
    m->norms[i * 3 + 2] = nz;
    m->uvs[i * 2 + 0] = u;
    m->uvs[i * 2 + 1] = v;
}

/*
 * Area-weighted vertex normals from the faces that share each vertex.
 *
 * Used when a file supplied none. Weighting by the cross product's length
 * rather than normalising first means a large face influences the result more
 * than a sliver, which is what stops thin triangles from skewing a surface.
 */
static bool derive_normals(MeshWidget *m)
{
    m->norms = (float *)calloc(m->vert_count * 3, sizeof(float));
    if (m->norms == NULL) {
        return false;
    }
    for (size_t t = 0; t < m->tri_count; t++) {
        const int *v = m->tris[t].v;
        const float *a = &m->verts[(size_t)v[0] * 3];
        const float *b = &m->verts[(size_t)v[1] * 3];
        const float *c = &m->verts[(size_t)v[2] * 3];

        float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
        float n[3] = { e1[1]*e2[2] - e1[2]*e2[1],
                       e1[2]*e2[0] - e1[0]*e2[2],
                       e1[0]*e2[1] - e1[1]*e2[0] };
        for (int k = 0; k < 3; k++) {
            for (int c2 = 0; c2 < 3; c2++) {
                m->norms[(size_t)v[k] * 3 + c2] += n[c2];
            }
        }
    }
    for (size_t i = 0; i < m->vert_count; i++) {
        float *n = &m->norms[i * 3];
        float l = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (l > 1e-12f) { n[0] /= l; n[1] /= l; n[2] /= l; }
        else            { n[0] = 0; n[1] = 0; n[2] = 1; }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Procedural primitives                                                      */
/* ------------------------------------------------------------------------- */

static bool gen_box(MeshWidget *m, size_t *vc, size_t *tc)
{
    static const float p[8][3] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    };
    /*
     * Six faces of four corners each, rather than eight shared corners.
     *
     * A cube cannot share vertices between faces once it carries attributes: a
     * corner belongs to three faces that disagree about both the normal and the
     * texture coordinate. Averaging them — which is what sharing forces — rounds
     * the cube's lighting and smears the texture diagonally across every edge.
     * Twenty-four vertices produce exactly the same twelve triangles.
     */
    /*
     * Wound so that (p1-p0) x (p2-p0) points *into* the box.
     *
     * That reads backwards and is worth stating plainly: y runs down the
     * screen, which flips the handedness of the projected signed area, so the
     * face the camera can see is the one whose 3D normal points away from it.
     * The sphere and torus follow the same rule. The box did not — its faces
     * were wound outward, so every front face was culled and what a box
     * actually showed was the inside of its far wall. With one mesh on screen
     * that is nearly invisible; put a second one behind it and the far object
     * draws straight through.
     */
    static const int face[6][4] = {
        {1,2,3,0}, {7,6,5,4},      /* -z, +z */
        {4,5,1,0}, {6,7,3,2},      /* -y, +y */
        {5,6,2,1}, {3,7,4,0},      /* +x, -x */
    };
    static const float nrm[6][3] = {
        {0,0,-1}, {0,0,1}, {0,-1,0}, {0,1,0}, {1,0,0}, {-1,0,0},
    };
    /* Each face carries the whole texture, in corner order. */
    static const float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };

    if (!ensure_attribs(m, 24)) return false;

    for (int f = 0; f < 6; f++) {
        int base = (int)m->vert_count;
        for (int k = 0; k < 4; k++) {
            const float *q = p[face[f][k]];
            if (!push_vert(m, vc, q[0], q[1], q[2])) return false;
            set_attrib(m, nrm[f][0], nrm[f][1], nrm[f][2], uv[k][0], uv[k][1]);
        }
        if (!push_tri(m, tc, base, base + 1, base + 2)) return false;
        if (!push_tri(m, tc, base, base + 2, base + 3)) return false;
    }
    return true;
}

static bool gen_sphere(MeshWidget *m, size_t *vc, size_t *tc, int seg, int ring)
{
    if (!ensure_attribs(m, (size_t)(ring + 1) * (seg + 1))) return false;
    for (int r = 0; r <= ring; r++) {
        float phi = (float)M_PI * (float)r / (float)ring;
        for (int sgm = 0; sgm <= seg; sgm++) {
            float th = 2.0f * (float)M_PI * (float)sgm / (float)seg;
            float x = sinf(phi) * cosf(th), y = cosf(phi), z = sinf(phi) * sinf(th);
            if (!push_vert(m, vc, x, y, z)) {
                return false;
            }
            /*
             * On a unit sphere the position is the normal.
             *
             * v is inverted because the two conventions disagree about which
             * way is up: an equirectangular map puts its north pole in the
             * first row, v = 0, while this renderer's y grows downwards, so
             * the sphere's v = 0 ring sits at the *bottom*. Without the flip
             * every planet map arrives upside down — which is easy to miss on
             * a gas giant and obvious the moment you texture the Earth.
             */
            set_attrib(m, x, y, z, (float)sgm / (float)seg,
                       1.0f - (float)r / (float)ring);
        }
    }
    for (int r = 0; r < ring; r++) {
        for (int sgm = 0; sgm < seg; sgm++) {
            int a = r * (seg + 1) + sgm;
            int b = a + seg + 1;
            /*
             * Wound so the cross product points *outward*.
             *
             * The earlier note here claimed the opposite and was wrong. It is
             * easy to be wrong about, because a closed body with the winding
             * reversed does not look broken: culling then keeps the far wall
             * and you see its inside, which for a sphere is the same silhouette
             * with a different half of the texture on it. Nothing gives it away
             * until something else depends on the normals — a light, which lit
             * the surface from behind, or a depth buffer, which put the far
             * wall in front of things it should have been behind.
             *
             * The test that settles it: with `cull` off the depth buffer alone
             * picks the nearest surface, which is the near wall by definition.
             * Culling must produce that same picture.
             */
            if (!push_tri(m, tc, a, b, a + 1)) return false;
            if (!push_tri(m, tc, a + 1, b, b + 1)) return false;
        }
    }
    return true;
}

static bool gen_torus(MeshWidget *m, size_t *vc, size_t *tc, int seg, int side, float r)
{
    if (!ensure_attribs(m, (size_t)(seg + 1) * (side + 1))) return false;
    for (int i = 0; i <= seg; i++) {
        float u = 2.0f * (float)M_PI * (float)i / (float)seg;
        for (int j = 0; j <= side; j++) {
            float v = 2.0f * (float)M_PI * (float)j / (float)side;
            float rr = 1.0f + r * cosf(v);
            if (!push_vert(m, vc, rr * cosf(u), r * sinf(v), rr * sinf(u))) {
                return false;
            }
            /* Outward from the tube's centre circle. */
            set_attrib(m, cosf(v) * cosf(u), sinf(v), cosf(v) * sinf(u),
                       (float)i / (float)seg, (float)j / (float)side);
        }
    }
    for (int i = 0; i < seg; i++) {
        for (int j = 0; j < side; j++) {
            int a = i * (side + 1) + j;
            int b = a + side + 1;
            /*
             * Wound so the cross product points *outward*.
             *
             * The earlier note here claimed the opposite and was wrong. It is
             * easy to be wrong about, because a closed body with the winding
             * reversed does not look broken: culling then keeps the far wall
             * and you see its inside, which for a sphere is the same silhouette
             * with a different half of the texture on it. Nothing gives it away
             * until something else depends on the normals — a light, which lit
             * the surface from behind, or a depth buffer, which put the far
             * wall in front of things it should have been behind.
             *
             * The test that settles it: with `cull` off the depth buffer alone
             * picks the nearest surface, which is the near wall by definition.
             * Culling must produce that same picture.
             */
            if (!push_tri(m, tc, a, b, a + 1)) return false;
            if (!push_tri(m, tc, a + 1, b, b + 1)) return false;
        }
    }
    return true;
}

static bool gen_cylinder(MeshWidget *m, size_t *vc, size_t *tc, int seg)
{
    /*
     * The caps get their own copies of the rim.
     *
     * A rim vertex is on the side wall, where the normal points outward, and on
     * the cap, where it points along y. One vertex cannot hold both, and sharing
     * them bevels the rim into a soft edge instead of a hard one.
     */
    if (!ensure_attribs(m, (size_t)(seg + 1) * 4 + 2)) return false;

    for (int i = 0; i <= seg; i++) {
        float th = 2.0f * (float)M_PI * (float)i / (float)seg;
        float cx = cosf(th), cz = sinf(th);
        float u = (float)i / (float)seg;

        if (!push_vert(m, vc, cx, -1.0f, cz)) return false;
        set_attrib(m, cx, 0.0f, cz, u, 0.0f);
        if (!push_vert(m, vc, cx,  1.0f, cz)) return false;
        set_attrib(m, cx, 0.0f, cz, u, 1.0f);
    }
    for (int i = 0; i < seg; i++) {
        int a = i * 2, b = a + 1, c = a + 2, d = a + 3;
        /* Same correction as the sphere: outward, so culling keeps the wall
         * facing the camera rather than the one behind it. */
        if (!push_tri(m, tc, a, c, b)) return false;
        if (!push_tri(m, tc, b, c, d)) return false;
    }

    /* Cap rims and centres, in their own vertices. */
    int rim_lo = (int)m->vert_count;
    for (int i = 0; i <= seg; i++) {
        float th = 2.0f * (float)M_PI * (float)i / (float)seg;
        float cx = cosf(th), cz = sinf(th);
        if (!push_vert(m, vc, cx, -1.0f, cz)) return false;
        set_attrib(m, 0.0f, -1.0f, 0.0f, 0.5f + 0.5f * cx, 0.5f + 0.5f * cz);
    }
    int rim_hi = (int)m->vert_count;
    for (int i = 0; i <= seg; i++) {
        float th = 2.0f * (float)M_PI * (float)i / (float)seg;
        float cx = cosf(th), cz = sinf(th);
        if (!push_vert(m, vc, cx, 1.0f, cz)) return false;
        set_attrib(m, 0.0f, 1.0f, 0.0f, 0.5f + 0.5f * cx, 0.5f + 0.5f * cz);
    }
    int c_lo = (int)m->vert_count;
    if (!push_vert(m, vc, 0, -1, 0)) return false;
    set_attrib(m, 0.0f, -1.0f, 0.0f, 0.5f, 0.5f);
    int c_hi = (int)m->vert_count;
    if (!push_vert(m, vc, 0,  1, 0)) return false;
    set_attrib(m, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f);

    for (int i = 0; i < seg; i++) {
        if (!push_tri(m, tc, c_lo, rim_lo + i + 1, rim_lo + i)) return false;
        if (!push_tri(m, tc, c_hi, rim_hi + i, rim_hi + i + 1)) return false;
    }
    return true;
}

/*
 * A flat annulus in the XZ plane: a planetary ring.
 *
 * The UVs are the reason this exists rather than being a textured plane. Ring
 * textures are a radial *strip* — one row of pixels from the inner edge to the
 * outer, repeated around — so u has to run along the radius and v around the
 * circumference. A square plane's corner-to-corner UVs would smear that strip
 * across the disc and lose the banding entirely.
 */
static bool gen_ring(MeshWidget *m, size_t *vc, size_t *tc, int seg, float inner)
{
    if (!ensure_attribs(m, (size_t)(seg + 1) * 2)) return false;

    for (int i = 0; i <= seg; i++) {
        float th = 2.0f * (float)M_PI * (float)i / (float)seg;
        float c = cosf(th), sn = sinf(th);
        float v = (float)i / (float)seg;

        if (!push_vert(m, vc, c * inner, 0.0f, sn * inner)) return false;
        set_attrib(m, 0.0f, 1.0f, 0.0f, 0.0f, v);
        if (!push_vert(m, vc, c, 0.0f, sn)) return false;
        set_attrib(m, 0.0f, 1.0f, 0.0f, 1.0f, v);
    }
    for (int i = 0; i < seg; i++) {
        int a = i * 2, b = a + 1, c = a + 2, d = a + 3;
        if (!push_tri(m, tc, a, b, c)) return false;
        if (!push_tri(m, tc, b, d, c)) return false;
    }
    return true;
}

static bool gen_plane(MeshWidget *m, size_t *vc, size_t *tc, int n)
{
    if (!ensure_attribs(m, (size_t)(n + 1) * (n + 1))) return false;

    for (int y = 0; y <= n; y++) {
        for (int x = 0; x <= n; x++) {
            float u = 2.0f * (float)x / (float)n - 1.0f;
            float v = 2.0f * (float)y / (float)n - 1.0f;
            if (!push_vert(m, vc, u, 0.0f, v)) return false;
            /* Flat, so every normal is the same; the plane is two-sided and the
             * lighting term takes the magnitude, so the sign does not matter. */
            set_attrib(m, 0.0f, 1.0f, 0.0f,
                       (float)x / (float)n, (float)y / (float)n);
        }
    }
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            int a = y * (n + 1) + x;
            int b = a + n + 1;
            if (!push_tri(m, tc, a, b, a + 1)) return false;
            if (!push_tri(m, tc, a + 1, b, b + 1)) return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* OBJ                                                                        */
/* ------------------------------------------------------------------------- */

/*
 * A deliberately small OBJ reader: `v` and `f` only.
 *
 * Normals and texture coordinates in the file are ignored — this renderer
 * shades from the face's own geometry and has no texturing, so reading them
 * would only be storing data nothing consumes. Faces with more than three
 * vertices are triangulated as a fan, which is correct for the convex polygons
 * exporters actually emit.
 */
/*
 * A small OBJ reader: `v`, `vt`, `vn`, `f`.
 *
 * OBJ indexes position, texture coordinate and normal separately, while a
 * renderer needs one array indexed once. So each distinct `v/vt/vn` triple
 * becomes one vertex, de-duplicated through a small hash — the standard
 * resolution, and the reason a cube exported with per-face normals arrives as
 * 24 vertices rather than 8.
 *
 * Polygons are triangulated as a fan, which is correct for the convex faces
 * exporters emit.
 */
typedef struct {
    int vi, ti, ni;
    int out;
} ObjKey;

static int obj_intern(ObjKey **tab, size_t *n, size_t *cap,
                      int vi, int ti, int ni, int *next)
{
    for (size_t k = 0; k < *n; k++) {
        if ((*tab)[k].vi == vi && (*tab)[k].ti == ti && (*tab)[k].ni == ni) {
            return (*tab)[k].out;
        }
    }
    if (*n == *cap) {
        size_t nc = (*cap == 0) ? 512 : *cap * 2;
        ObjKey *g = (ObjKey *)realloc(*tab, nc * sizeof(ObjKey));
        if (g == NULL) {
            return -1;
        }
        *tab = g;
        *cap = nc;
    }
    (*tab)[*n].vi = vi; (*tab)[*n].ti = ti; (*tab)[*n].ni = ni;
    (*tab)[*n].out = *next;
    (*n)++;
    return (*next)++;
}

static bool load_obj(MeshWidget *m, size_t *vc, size_t *tc)
{
    FILE *f = fopen(m->path, "r");
    if (f == NULL) {
        fprintf(stderr, "error: cannot open mesh '%s'.\n", m->path);
        return false;
    }

    float *P = NULL, *T = NULL, *N = NULL;
    size_t np = 0, nt = 0, nn = 0, cp = 0, ct = 0, cn = 0;

    ObjKey *keys = NULL;
    size_t  nk = 0, ck = 0;
    int     next_out = 0;
    bool    ok = true;

    char line[2048];
    while (ok && fgets(line, sizeof line, f) != NULL) {
        if (line[0] == 'v' && line[1] == ' ') {
            if (np == cp) {
                cp = cp ? cp * 2 : 512;
                float *g = (float *)realloc(P, cp * 3 * sizeof(float));
                if (!g) { ok = false; break; }
                P = g;
            }
            if (sscanf(line + 2, "%f %f %f", &P[np*3], &P[np*3+1], &P[np*3+2]) == 3) np++;
        } else if (line[0] == 'v' && line[1] == 't') {
            if (nt == ct) {
                ct = ct ? ct * 2 : 512;
                float *g = (float *)realloc(T, ct * 2 * sizeof(float));
                if (!g) { ok = false; break; }
                T = g;
            }
            if (sscanf(line + 3, "%f %f", &T[nt*2], &T[nt*2+1]) >= 1) nt++;
        } else if (line[0] == 'v' && line[1] == 'n') {
            if (nn == cn) {
                cn = cn ? cn * 2 : 512;
                float *g = (float *)realloc(N, cn * 3 * sizeof(float));
                if (!g) { ok = false; break; }
                N = g;
            }
            if (sscanf(line + 3, "%f %f %f", &N[nn*3], &N[nn*3+1], &N[nn*3+2]) == 3) nn++;
        } else if (line[0] == 'f' && (line[1] == ' ' || line[1] == '\t')) {
            int idx[64], cnt = 0;
            const char *p = line + 1;

            while (*p != '\0' && cnt < 64) {
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '\0' || *p == '\n' || *p == '\r') break;

                int vi = 0, ti = 0, ni = 0;
                vi = atoi(p);
                const char *sl = p;
                while (*sl && *sl != ' ' && *sl != '\t' && *sl != '\n' && *sl != '\r') {
                    if (*sl == '/') {
                        sl++;
                        if (*sl == '/') { ni = atoi(sl + 1); break; }
                        ti = atoi(sl);
                        const char *sl2 = sl;
                        while (*sl2 && *sl2 != '/' && *sl2 != ' ' && *sl2 != '\t') sl2++;
                        if (*sl2 == '/') ni = atoi(sl2 + 1);
                        break;
                    }
                    sl++;
                }

                /* OBJ is 1-based; negative counts back from the end. */
                int rv = (vi > 0) ? vi - 1 : (int)np + vi;
                int rt = (ti > 0) ? ti - 1 : (ti < 0 ? (int)nt + ti : -1);
                int rn = (ni > 0) ? ni - 1 : (ni < 0 ? (int)nn + ni : -1);

                if (rv >= 0 && (size_t)rv < np) {
                    int out = obj_intern(&keys, &nk, &ck, rv, rt, rn, &next_out);
                    if (out < 0) { ok = false; break; }
                    idx[cnt++] = out;
                }
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
            }
            for (int k = 2; k < cnt; k++) {
                if (!push_tri(m, tc, idx[0], idx[k - 1], idx[k])) { ok = false; break; }
            }
        }
    }
    fclose(f);

    if (ok && nk > 0) {
        ok = ensure_attribs(m, nk);
    }
    if (ok) {
        bool have_n = false, have_t = false;
        for (size_t k = 0; k < nk; k++) {
            const ObjKey *K = &keys[k];
            if (!push_vert(m, vc, P[K->vi*3], P[K->vi*3+1], P[K->vi*3+2])) { ok = false; break; }

            float nx = 0, ny = 0, nz = 1, u = 0, v = 0;
            if (K->ni >= 0 && (size_t)K->ni < nn) {
                nx = N[K->ni*3]; ny = N[K->ni*3+1]; nz = N[K->ni*3+2];
                have_n = true;
            }
            if (K->ti >= 0 && (size_t)K->ti < nt) {
                u = T[K->ti*2]; v = T[K->ti*2+1];
                have_t = true;
            }
            set_attrib(m, nx, ny, nz, u, v);
        }
        if (!have_n) { free(m->norms); m->norms = NULL; }
        if (!have_t) { free(m->uvs);   m->uvs   = NULL; }
    }

    free(P); free(T); free(N); free(keys);
    return ok;
}

/* ------------------------------------------------------------------------- */

bool mesh_load(MeshWidget *m)
{
    if (m == NULL) {
        return false;
    }

    size_t vc = 0, tc = 0;
    bool   ok;

    if (m->path != NULL) {
        /* Chosen by extension rather than by sniffing the contents: a .gltf is
         * JSON and a .glb has a magic number, but an OBJ has neither, so the
         * name is the only thing all three agree on. */
        size_t n = strlen(m->path);
        bool is_gltf = (n > 5 && strcasecmp(m->path + n - 5, ".gltf") == 0) ||
                       (n > 4 && strcasecmp(m->path + n - 4, ".glb")  == 0);
        ok = is_gltf ? vr_mesh_load_gltf(m, &vc, &tc) : load_obj(m, &vc, &tc);
    } else {
        const char *sh = (m->shape != NULL) ? m->shape : "box";
        if      (strcmp(sh, "box") == 0)      ok = gen_box(m, &vc, &tc);
        else if (strcmp(sh, "cube") == 0)     ok = gen_box(m, &vc, &tc);
        else if (strcmp(sh, "sphere") == 0)   ok = gen_sphere(m, &vc, &tc, 28, 18);
        else if (strcmp(sh, "torus") == 0)    ok = gen_torus(m, &vc, &tc, 36, 18, 0.38f);
        else if (strcmp(sh, "cylinder") == 0) ok = gen_cylinder(m, &vc, &tc, 30);
        else if (strcmp(sh, "plane") == 0)    ok = gen_plane(m, &vc, &tc, 12);
        else if (strcmp(sh, "ring") == 0)     ok = gen_ring(m, &vc, &tc, 96, 0.55f);
        else {
            fprintf(stderr, "warning: unknown mesh shape '%s' — using a box.\n", sh);
            ok = gen_box(m, &vc, &tc);
        }
    }

    if (!ok || m->vert_count == 0 || m->tri_count == 0) {
        fprintf(stderr, "error: mesh '%s' produced no geometry.\n",
                m->base.id ? m->base.id : "(unnamed)");
        mesh_free(m);
        return false;
    }

    /* Drop triangles that reference vertices the file never defined, rather
     * than reading past the array later. */
    size_t kept = 0;
    for (size_t i = 0; i < m->tri_count; i++) {
        const MeshTri *t = &m->tris[i];
        if (t->v[0] >= 0 && (size_t)t->v[0] < m->vert_count &&
            t->v[1] >= 0 && (size_t)t->v[1] < m->vert_count &&
            t->v[2] >= 0 && (size_t)t->v[2] < m->vert_count) {
            m->tris[kept++] = *t;
        }
    }
    if (kept != m->tri_count) {
        fprintf(stderr, "warning: mesh '%s' — dropped %zu triangle(s) with bad indices.\n",
                m->base.id ? m->base.id : "(unnamed)", m->tri_count - kept);
        m->tri_count = kept;
    }

    /*
     * Normalise into a unit cube about the origin.
     *
     * Without this, `size` would mean something different for every file: an
     * OBJ in millimetres and one in metres differ by a thousand, and many are
     * modelled far from the origin so they would rotate about a point outside
     * themselves.
     */
    float mn[3] = { 1e30f, 1e30f, 1e30f };
    float mx[3] = { -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < m->vert_count; i++) {
        for (int k = 0; k < 3; k++) {
            float v = m->verts[i * 3 + k];
            if (v < mn[k]) mn[k] = v;
            if (v > mx[k]) mx[k] = v;
        }
    }

    float ext = 0.0f;
    for (int k = 0; k < 3; k++) {
        float e = mx[k] - mn[k];
        if (e > ext) ext = e;
    }
    if (ext <= 0.0f) {
        ext = 1.0f;
    }
    float inv = 2.0f / ext;      /* longest axis spans -1..1 */

    for (size_t i = 0; i < m->vert_count; i++) {
        for (int k = 0; k < 3; k++) {
            m->verts[i * 3 + k] = (m->verts[i * 3 + k] - (mn[k] + mx[k]) * 0.5f) * inv;
        }
    }

    /* A mesh that came without normals still needs them for smooth shading. */
    if (m->norms == NULL && !derive_normals(m)) {
        fprintf(stderr, "warning: mesh '%s' — could not derive normals.\n",
                m->base.id ? m->base.id : "(unnamed)");
    }

    /*
     * Normals were computed in model space and the mesh has just been scaled
     * uniformly, so they stay correct — a uniform scale does not shear.
     */
    fprintf(stderr, "mesh: %s — %zu vertices, %zu triangles%s%s\n",
            m->path ? m->path : (m->shape ? m->shape : "box"),
            m->vert_count, m->tri_count,
            m->norms ? ", normals" : "", m->uvs ? ", uvs" : "");
    return true;
}

void mesh_free(MeshWidget *m)
{
    if (m == NULL) {
        return;
    }
    free(m->verts);
    free(m->norms);
    free(m->uvs);
    free(m->tris);
    m->verts = NULL;
    m->norms = NULL;
    m->uvs = NULL;
    m->tris = NULL;
    m->vert_count = 0;
    m->tri_count = 0;
}
