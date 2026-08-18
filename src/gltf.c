/*
 * gltf.c — a glTF 2.0 reader.
 *
 * Handles both containers: .gltf, where the JSON sits in the file and the
 * binary data lives either in a sibling .bin or in a base64 data: URI, and
 * .glb, where JSON and binary are chunks of one file.
 *
 * What it reads is deliberately narrow — positions, normals, texture
 * coordinates, triangle indices, the node transforms that place them, and the
 * base colour texture. That is exactly the subset this renderer can draw. The
 * rest of glTF (animation, skinning, morph targets, the PBR material model,
 * cameras, extensions) is skipped, and the loader says so rather than pretending
 * a skinned model imported correctly.
 *
 * The one structural point worth knowing: glTF stores geometry in accessors,
 * which read through bufferViews into buffers, and any of those may be strided
 * or share storage with another. So everything goes through acc_read(), which
 * turns "component c of element i of accessor a" into a float regardless of how
 * it was packed. Reading the buffers directly would work for files written by
 * one exporter and break on the next.
 */

#include "mesh_internal.h"
#include "cJSON.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static int json_int_of(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static float json_float_of(const cJSON *o, const char *k, float def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

/* 0..1 as a colour byte. The emissive factor is a float in the file and a
 * Color in the widget, and this is the whole of the conversion. */
static unsigned char byte_of(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

static const char *json_str_of(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* The i-th element of an array, or NULL when the index is out of range —
 * which is how a file referring to an accessor that does not exist is caught. */
static const cJSON *at(const cJSON *arr, int i)
{
    if (!cJSON_IsArray(arr) || i < 0) {
        return NULL;
    }
    return cJSON_GetArrayItem(arr, i);
}

static char *read_whole_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);

    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* "dir/model.gltf" + "textures/a.png" → "dir/textures/a.png". An absolute
 * reference is returned unchanged. */
static char *sibling_path(const char *base, const char *rel)
{
    if (rel == NULL) {
        return NULL;
    }
    if (rel[0] == '/') {
        return strdup(rel);
    }
    const char *slash = strrchr(base, '/');
    size_t dir = (slash != NULL) ? (size_t)(slash - base) + 1 : 0;

    char *out = (char *)malloc(dir + strlen(rel) + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, base, dir);
    strcpy(out + dir, rel);
    return out;
}

/*
 * Percent-decoding, in place.
 *
 * URIs in a glTF are URI-encoded, so a file called "red brick.png" arrives as
 * "red%20brick.png" and would not open. Only the escapes are undone; the path
 * separators are already what we want.
 */
static void uri_unescape(char *s)
{
    char *w = s;
    for (const char *r = s; *r != '\0'; r++) {
        if (r[0] == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* ------------------------------------------------------------------------- */
/* base64                                                                     */
/* ------------------------------------------------------------------------- */

static unsigned char *base64_decode(const char *in, size_t *out_len)
{
    static const signed char T[256] = {
        ['A']= 0,['B']= 1,['C']= 2,['D']= 3,['E']= 4,['F']= 5,['G']= 6,['H']= 7,
        ['I']= 8,['J']= 9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63,
    };

    size_t n = strlen(in);
    unsigned char *out = (unsigned char *)malloc(n / 4 * 3 + 4);
    if (out == NULL) {
        return NULL;
    }

    unsigned int acc = 0;
    int bits = 0;
    size_t w = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') {
            break;
        }
        /* Whitespace is legal inside a base64 payload and 'A' decodes to 0, so
         * the table alone cannot tell padding from a real character. */
        if (c != 'A' && T[c] == 0) {
            continue;
        }
        acc = (acc << 6) | (unsigned int)T[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[w++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    *out_len = w;
    return out;
}

/* ------------------------------------------------------------------------- */
/* The document                                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    unsigned char *data;
    size_t         len;
    bool           owned;   /* false for the GLB chunk, which the file owns */
} GltfBuffer;

typedef struct {
    cJSON       *root;
    GltfBuffer  *buffers;
    int          buffer_count;
    const cJSON *views, *accessors, *meshes, *nodes, *materials, *textures, *images;
    const char  *path;      /* for resolving sibling files and for messages   */
} Gltf;

static void gltf_release(Gltf *g)
{
    for (int i = 0; i < g->buffer_count; i++) {
        if (g->buffers[i].owned) {
            free(g->buffers[i].data);
        }
    }
    free(g->buffers);
    cJSON_Delete(g->root);
}

/*
 * Resolves every buffer to bytes.
 *
 * Three sources, in the order the spec allows them: the GLB binary chunk (no
 * uri at all), an embedded base64 data: URI, and a sibling file.
 */
static bool load_buffers(Gltf *g, unsigned char *bin, size_t bin_len)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(g->root, "buffers");
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;

    g->buffers = (GltfBuffer *)calloc((size_t)(n > 0 ? n : 1), sizeof(GltfBuffer));
    if (g->buffers == NULL) {
        return false;
    }
    g->buffer_count = n;

    for (int i = 0; i < n; i++) {
        const cJSON *b = at(arr, i);
        const char *uri = json_str_of(b, "uri");

        if (uri == NULL) {
            g->buffers[i].data  = bin;
            g->buffers[i].len   = bin_len;
            g->buffers[i].owned = false;
            if (bin == NULL) {
                fprintf(stderr, "warning: glTF '%s' — buffer %d has no uri and the "
                                "file has no binary chunk.\n", g->path, i);
            }
            continue;
        }

        if (strncmp(uri, "data:", 5) == 0) {
            const char *comma = strchr(uri, ',');
            if (comma == NULL) {
                fprintf(stderr, "warning: glTF '%s' — malformed data: uri.\n", g->path);
                continue;
            }
            g->buffers[i].data  = base64_decode(comma + 1, &g->buffers[i].len);
            g->buffers[i].owned = true;
            continue;
        }

        char *side = sibling_path(g->path, uri);
        if (side == NULL) {
            return false;
        }
        uri_unescape(side);

        size_t len = 0;
        g->buffers[i].data  = (unsigned char *)read_whole_file(side, &len);
        g->buffers[i].len   = len;
        g->buffers[i].owned = true;
        if (g->buffers[i].data == NULL) {
            fprintf(stderr, "warning: glTF '%s' — cannot read buffer '%s'.\n",
                    g->path, side);
        }
        free(side);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Accessors                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    const unsigned char *base;   /* first byte of element 0                  */
    size_t   stride;             /* bytes between elements                   */
    int      comp_type;          /* glTF componentType                       */
    int      comps;              /* 1 for SCALAR, 2 for VEC2, 3 for VEC3 ... */
    int      count;
    bool     normalized;
    size_t   avail;              /* bytes left in the buffer, for bounds     */
} Accessor;

static int comp_size(int t)
{
    switch (t) {
        case 5120: case 5121: return 1;   /* byte, unsigned byte   */
        case 5122: case 5123: return 2;   /* short, unsigned short */
        case 5125: case 5126: return 4;   /* unsigned int, float   */
        default:              return 0;
    }
}

static int type_comps(const char *t)
{
    if (t == NULL)                  return 0;
    if (strcmp(t, "SCALAR") == 0)   return 1;
    if (strcmp(t, "VEC2") == 0)     return 2;
    if (strcmp(t, "VEC3") == 0)     return 3;
    if (strcmp(t, "VEC4") == 0)     return 4;
    if (strcmp(t, "MAT4") == 0)     return 16;
    return 0;
}

static bool acc_open(const Gltf *g, int index, Accessor *a)
{
    const cJSON *ac = at(g->accessors, index);
    if (ac == NULL) {
        return false;
    }

    a->comp_type  = json_int_of(ac, "componentType", 0);
    a->comps      = type_comps(json_str_of(ac, "type"));
    a->count      = json_int_of(ac, "count", 0);
    a->normalized = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ac, "normalized"));

    int csz = comp_size(a->comp_type);
    if (csz == 0 || a->comps == 0 || a->count <= 0) {
        return false;
    }

    /*
     * A sparse accessor stores a dense base plus a list of overrides. Reading
     * only the base would silently produce the wrong geometry, so this refuses
     * rather than guessing.
     */
    if (cJSON_GetObjectItemCaseSensitive(ac, "sparse") != NULL) {
        fprintf(stderr, "warning: glTF '%s' — sparse accessors are not supported.\n",
                g->path);
        return false;
    }

    const cJSON *bv = at(g->views, json_int_of(ac, "bufferView", -1));
    if (bv == NULL) {
        return false;      /* a view-less accessor reads as zeros; skip it */
    }

    int bi = json_int_of(bv, "buffer", -1);
    if (bi < 0 || bi >= g->buffer_count || g->buffers[bi].data == NULL) {
        return false;
    }

    size_t off = (size_t)json_int_of(bv, "byteOffset", 0)
               + (size_t)json_int_of(ac, "byteOffset", 0);
    size_t packed = (size_t)csz * (size_t)a->comps;
    size_t stride = (size_t)json_int_of(bv, "byteStride", 0);

    a->stride = (stride > 0) ? stride : packed;

    if (off >= g->buffers[bi].len) {
        return false;
    }
    a->base  = g->buffers[bi].data + off;
    a->avail = g->buffers[bi].len - off;

    /* The last element must fit: a truncated .bin otherwise reads past the end. */
    size_t need = a->stride * (size_t)(a->count - 1) + packed;
    if (need > a->avail) {
        fprintf(stderr, "warning: glTF '%s' — accessor %d runs past its buffer.\n",
                g->path, index);
        return false;
    }
    return true;
}

/* One component, as a float. Integer types are returned as-is unless the
 * accessor is `normalized`, which maps them onto 0..1 (or -1..1 when signed) —
 * the convention exporters use for compressed UVs and colours. */
static float acc_read(const Accessor *a, int elem, int comp)
{
    if (comp >= a->comps) {
        return 0.0f;
    }
    const unsigned char *p = a->base + a->stride * (size_t)elem
                           + (size_t)comp_size(a->comp_type) * (size_t)comp;

    switch (a->comp_type) {
        case 5126: { float v; memcpy(&v, p, 4); return v; }
        case 5125: { uint32_t v; memcpy(&v, p, 4); return (float)v; }
        case 5123: { uint16_t v; memcpy(&v, p, 2);
                     return a->normalized ? (float)v / 65535.0f : (float)v; }
        case 5122: { int16_t v; memcpy(&v, p, 2);
                     return a->normalized ? fmaxf((float)v / 32767.0f, -1.0f) : (float)v; }
        case 5121: { uint8_t v = *p;
                     return a->normalized ? (float)v / 255.0f : (float)v; }
        case 5120: { int8_t v; memcpy(&v, p, 1);
                     return a->normalized ? fmaxf((float)v / 127.0f, -1.0f) : (float)v; }
        default:   return 0.0f;
    }
}

/* ------------------------------------------------------------------------- */
/* Node transforms                                                            */
/* ------------------------------------------------------------------------- */

/* Column-major 4x4, as glTF stores them. m[c * 4 + r]. */
static void mat_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_mul(const float *a, const float *b, float *out)
{
    float t[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1]
                         + a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    memcpy(out, t, sizeof t);
}

/* A node carries either an explicit matrix or a translation/rotation/scale
 * triple — never both, per the spec, but a file that supplies both is handled
 * by preferring the matrix. */
static void node_local(const cJSON *node, float *m)
{
    const cJSON *mat = cJSON_GetObjectItemCaseSensitive(node, "matrix");
    if (cJSON_IsArray(mat) && cJSON_GetArraySize(mat) == 16) {
        for (int i = 0; i < 16; i++) {
            const cJSON *v = cJSON_GetArrayItem(mat, i);
            m[i] = cJSON_IsNumber(v) ? (float)v->valuedouble : 0.0f;
        }
        return;
    }

    float t[3] = { 0, 0, 0 };
    float q[4] = { 0, 0, 0, 1 };
    float s[3] = { 1, 1, 1 };

    const struct { const char *key; float *dst; int n; } src[] = {
        { "translation", t, 3 }, { "rotation", q, 4 }, { "scale", s, 3 },
    };
    for (size_t k = 0; k < sizeof src / sizeof src[0]; k++) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(node, src[k].key);
        if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < src[k].n) {
            continue;
        }
        for (int i = 0; i < src[k].n; i++) {
            const cJSON *v = cJSON_GetArrayItem(a, i);
            if (cJSON_IsNumber(v)) {
                src[k].dst[i] = (float)v->valuedouble;
            }
        }
    }

    /* Quaternion (x,y,z,w) to a rotation matrix, then scale the columns. */
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[9] = {
        1 - 2 * (y * y + z * z),     2 * (x * y + z * w),     2 * (x * z - y * w),
            2 * (x * y - z * w), 1 - 2 * (x * x + z * z),     2 * (y * z + x * w),
            2 * (x * z + y * w),     2 * (y * z - x * w), 1 - 2 * (x * x + y * y),
    };

    mat_identity(m);
    for (int c = 0; c < 3; c++) {
        for (int rr = 0; rr < 3; rr++) {
            m[c * 4 + rr] = r[c * 3 + rr] * s[c];
        }
    }
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
}

/* ------------------------------------------------------------------------- */
/* Geometry                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    MeshWidget *m;
    size_t     *vc, *tc;
    size_t      attrib_cap;
    int         skipped_modes;
    int         tex_source;      /* image index of the first base colour map */
    int         ao_source;       /* ...and of the first occlusion map        */
    int         nrm_source;      /* ...and of the first normal map           */
    int         emis_source;     /* ...and of the first emissive map         */
    bool        ao_packed;       /* it came from an ARM map, not a declared one */
    float       normal_scale;    /* normalTexture.scale, 1 when unstated     */
    float       emissive[3];     /* emissiveFactor, black when unstated      */
    /*
     * Set when any primitive arrived without TANGENT.
     *
     * All or nothing: a file that supplies tangents for some primitives and not
     * others would otherwise get a frame on part of the model and zeroes on the
     * rest, which reads as one half of an object lit correctly and the other
     * half black. Deriving the lot from the UVs is both consistent and close
     * enough — the exporter derived its own from the same parameterisation.
     */
    bool        tan_missing;
} Build;

/* Grows the attribute arrays geometrically. They are indexed by vertex, so
 * they must always be at least as long as the vertex array. */
static bool reserve_attribs(Build *bld, size_t need)
{
    if (need <= bld->attrib_cap) {
        return true;
    }
    size_t nc = (bld->attrib_cap == 0) ? 256 : bld->attrib_cap;
    while (nc < need) {
        nc *= 2;
    }
    if (!vr_mesh_ensure_attribs(bld->m, nc)) {
        return false;
    }
    bld->attrib_cap = nc;
    return true;
}

static void note_texture(const Gltf *g, Build *bld, int material)
{
    if ((bld->tex_source >= 0 && bld->ao_source >= 0 &&
         bld->nrm_source >= 0 && bld->emis_source >= 0) || material < 0) {
        return;
    }
    const cJSON *mat = at(g->materials, material);
    if (mat == NULL) {
        return;
    }
    /*
     * Occlusion first, because it is often the same image as the metallic /
     * roughness map — the ARM packing — and a material may name that one
     * without naming a base colour at all.
     */
    const cJSON *occ = cJSON_GetObjectItemCaseSensitive(mat, "occlusionTexture");
    if (occ != NULL) {
        const cJSON *ot = at(g->textures, json_int_of(occ, "index", -1));
        if (ot != NULL) {
            bld->ao_source = json_int_of(ot, "source", -1);
        }
    }

    const cJSON *pbr = cJSON_GetObjectItemCaseSensitive(mat, "pbrMetallicRoughness");

    /*
     * No occlusionTexture? Try the metallic/roughness map's red channel.
     *
     * glTF reserves red and alpha there as unused, which is exactly why the
     * "ARM" packing — occlusion, roughness, metalness in R, G, B — became the
     * common way to ship three maps as one image. Plenty of exporters, Poly
     * Haven's among them, pack that way and then never declare the occlusion
     * slot, so following the declaration alone finds nothing.
     *
     * This is a convention rather than a guarantee, so it is not trusted
     * blindly: the loader checks that the channel actually looks like occlusion
     * before using it, and drops it if not.
     */
    if (bld->ao_source < 0 && pbr != NULL) {
        const cJSON *mrt = cJSON_GetObjectItemCaseSensitive(pbr, "metallicRoughnessTexture");
        if (mrt != NULL) {
            const cJSON *mt = at(g->textures, json_int_of(mrt, "index", -1));
            if (mt != NULL) {
                bld->ao_source = json_int_of(mt, "source", -1);
                bld->ao_packed = true;
            }
        }
    }

    /*
     * The normal map, and with it the one material scalar worth reading: glTF's
     * normalTexture.scale, which the exporter sets when the map was baked
     * stronger or weaker than the surface should actually look.
     */
    if (bld->nrm_source < 0) {
        const cJSON *nt = cJSON_GetObjectItemCaseSensitive(mat, "normalTexture");
        if (nt != NULL) {
            const cJSON *tt = at(g->textures, json_int_of(nt, "index", -1));
            if (tt != NULL) {
                bld->nrm_source  = json_int_of(tt, "source", -1);
                bld->normal_scale = json_float_of(nt, "scale", 1.0f);
            }
        }
    }

    /*
     * Emissive. The factor is read even without a map, because a material that
     * glows uniformly — a bare filament, a painted-on indicator — states it
     * with the factor alone and has no texture at all.
     */
    if (bld->emis_source < 0) {
        const cJSON *ef = cJSON_GetObjectItemCaseSensitive(mat, "emissiveFactor");
        if (cJSON_IsArray(ef) && cJSON_GetArraySize(ef) >= 3) {
            for (int k = 0; k < 3; k++) {
                const cJSON *c = cJSON_GetArrayItem(ef, k);
                if (cJSON_IsNumber(c)) {
                    bld->emissive[k] = (float)c->valuedouble;
                }
            }
        }
        const cJSON *et = cJSON_GetObjectItemCaseSensitive(mat, "emissiveTexture");
        if (et != NULL) {
            const cJSON *tt = at(g->textures, json_int_of(et, "index", -1));
            if (tt != NULL) {
                bld->emis_source = json_int_of(tt, "source", -1);
                /*
                 * A material with an emissive texture and no factor still
                 * glows: the spec's default factor is black, but exporters that
                 * omit it while naming a map plainly mean the map. Treating
                 * that as "off" would drop every lit screen in the file.
                 */
                if (bld->emissive[0] == 0.0f && bld->emissive[1] == 0.0f &&
                    bld->emissive[2] == 0.0f) {
                    bld->emissive[0] = bld->emissive[1] = bld->emissive[2] = 1.0f;
                }
            }
        }
    }

    const cJSON *bct = (pbr != NULL)
        ? cJSON_GetObjectItemCaseSensitive(pbr, "baseColorTexture") : NULL;
    if (bct == NULL) {
        return;
    }
    const cJSON *tex = at(g->textures, json_int_of(bct, "index", -1));
    if (tex == NULL) {
        return;
    }
    bld->tex_source = json_int_of(tex, "source", -1);
}

/*
 * One primitive: its vertices transformed into place, then its triangles.
 *
 * Triangles are the only mode drawn. Strips and fans are convertible, but points
 * and lines are not — this rasterizer fills triangles and nothing else — so the
 * unsupported ones are counted and reported once rather than per primitive.
 */
static bool add_primitive(const Gltf *g, Build *bld, const cJSON *prim, const float *xf)
{
    int mode = json_int_of(prim, "mode", 4);
    if (mode != 4) {
        bld->skipped_modes++;
        return true;
    }

    const cJSON *attr = cJSON_GetObjectItemCaseSensitive(prim, "attributes");
    if (attr == NULL) {
        return true;
    }

    Accessor pos, nrm, uv, tan;
    if (!acc_open(g, json_int_of(attr, "POSITION", -1), &pos)) {
        return true;              /* no positions: nothing to draw */
    }
    bool has_n = acc_open(g, json_int_of(attr, "NORMAL", -1), &nrm)
                 && nrm.count >= pos.count;
    bool has_uv = acc_open(g, json_int_of(attr, "TEXCOORD_0", -1), &uv)
                 && uv.count >= pos.count;
    /*
     * TANGENT is a vec4: the direction of increasing u, and a handedness in w.
     *
     * Worth reading rather than always deriving, because an exporter's
     * tangents are the ones the normal map was baked against — usually
     * MikkTSpace, whose whole purpose is that the baker and the renderer agree.
     * Deriving from the UVs lands very close, but "very close" is visible as a
     * faint shift in where a bevel catches the light.
     */
    bool has_tan = acc_open(g, json_int_of(attr, "TANGENT", -1), &tan)
                 && tan.count >= pos.count && tan.comps >= 4;

    /*
     * Normals transform by the inverse transpose, which for the rigid and
     * uniformly-scaled transforms exporters actually emit is the same rotation
     * as the positions, up to a scale that renormalising removes. Non-uniform
     * scale would shear them slightly; that is a deliberate simplification, and
     * the visible cost is a small lighting error on such nodes only.
     */
    int base = (int)bld->m->vert_count;

    if (!reserve_attribs(bld, (size_t)base + (size_t)pos.count)) {
        return false;
    }
    /*
     * Grown whenever the array exists at all, not only when this primitive has
     * tangents: the array is indexed by vertex, so a primitive that contributes
     * none still has to leave room for its own vertices or the next one writes
     * past the end.
     */
    if ((bld->m->tans != NULL || has_tan) &&
        !vr_mesh_ensure_tangents(bld->m, (size_t)base + (size_t)pos.count)) {
        return false;
    }
    if (!has_tan) {
        bld->tan_missing = true;
    }

    for (int i = 0; i < pos.count; i++) {
        float x = acc_read(&pos, i, 0);
        float y = acc_read(&pos, i, 1);
        float z = acc_read(&pos, i, 2);

        float wx = xf[0] * x + xf[4] * y + xf[8]  * z + xf[12];
        float wy = xf[1] * x + xf[5] * y + xf[9]  * z + xf[13];
        float wz = xf[2] * x + xf[6] * y + xf[10] * z + xf[14];

        if (!vr_mesh_push_vert(bld->m, bld->vc, wx, wy, wz)) {
            return false;
        }

        float nx = 0.0f, ny = 0.0f, nz = 1.0f;
        if (has_n) {
            float ax = acc_read(&nrm, i, 0);
            float ay = acc_read(&nrm, i, 1);
            float az = acc_read(&nrm, i, 2);
            nx = xf[0] * ax + xf[4] * ay + xf[8]  * az;
            ny = xf[1] * ax + xf[5] * ay + xf[9]  * az;
            nz = xf[2] * ax + xf[6] * ay + xf[10] * az;
            float l = sqrtf(nx * nx + ny * ny + nz * nz);
            if (l > 1e-12f) { nx /= l; ny /= l; nz /= l; }
        }
        vr_mesh_set_attrib(bld->m, nx, ny, nz,
                           has_uv ? acc_read(&uv, i, 0) : 0.0f,
                           has_uv ? acc_read(&uv, i, 1) : 0.0f);

        if (bld->m->tans != NULL) {
            float tx = 0.0f, ty = 0.0f, tz = 0.0f, tw = 0.0f;
            if (has_tan) {
                float bx = acc_read(&tan, i, 0);
                float by = acc_read(&tan, i, 1);
                float bz = acc_read(&tan, i, 2);
                /* A tangent is a direction along the surface, so it takes the
                 * transform itself rather than its inverse transpose — the
                 * translation column is simply not applied. */
                tx = xf[0] * bx + xf[4] * by + xf[8]  * bz;
                ty = xf[1] * bx + xf[5] * by + xf[9]  * bz;
                tz = xf[2] * bx + xf[6] * by + xf[10] * bz;
                float l = sqrtf(tx * tx + ty * ty + tz * tz);
                if (l > 1e-12f) { tx /= l; ty /= l; tz /= l; }
                /* The handedness survives the node transform untouched: every
                 * transform on the way in here is a rotation composed with a
                 * positive scale, and neither reverses a cross product. */
                tw = (acc_read(&tan, i, 3) < 0.0f) ? -1.0f : 1.0f;
            }
            vr_mesh_set_tangent(bld->m, tx, ty, tz, tw);
        }
    }

    /*
     * Winding is reversed on the way in.
     *
     * glTF calls a face front-facing when it is counter-clockwise seen from
     * outside, which puts its geometric normal on the viewer's side. This
     * renderer keeps the opposite: because y runs down the screen, the
     * projected signed area changes sign, and the face you can see is the one
     * whose normal points away. The half turn applied to the scene root is a
     * rotation and so preserves winding, which means nothing else corrects for
     * this — imported with glTF's winding and `cull` left at its default, a
     * model disappears completely.
     *
     * Reversing here rather than mirroring the geometry matters: a mirror would
     * also flip the model, and lettering on a product would read backwards.
     */
    Accessor idx;
    if (acc_open(g, json_int_of(prim, "indices", -1), &idx)) {
        for (int i = 0; i + 2 < idx.count; i += 3) {
            int a = base + (int)acc_read(&idx, i,     0);
            int b = base + (int)acc_read(&idx, i + 1, 0);
            int c = base + (int)acc_read(&idx, i + 2, 0);
            if (!vr_mesh_push_tri(bld->m, bld->tc, a, c, b)) {
                return false;
            }
        }
    } else {
        /* Indices are optional: without them the positions are the triangles. */
        for (int i = 0; i + 2 < pos.count; i += 3) {
            if (!vr_mesh_push_tri(bld->m, bld->tc, base + i, base + i + 2, base + i + 1)) {
                return false;
            }
        }
    }

    note_texture(g, bld, json_int_of(prim, "material", -1));
    return true;
}

/*
 * Walks the node hierarchy, accumulating transforms.
 *
 * Depth is bounded because a malformed file can contain a cycle — nodes
 * reference children by index, and nothing in the format prevents a loop, which
 * would otherwise recurse until the stack runs out.
 */
static bool walk_node(const Gltf *g, Build *bld, int index, const float *parent, int depth)
{
    if (depth > 64) {
        return true;
    }
    const cJSON *node = at(g->nodes, index);
    if (node == NULL) {
        return true;
    }

    float local[16], world[16];
    node_local(node, local);
    mat_mul(parent, local, world);

    int mi = json_int_of(node, "mesh", -1);
    if (mi >= 0) {
        const cJSON *mesh = at(g->meshes, mi);
        const cJSON *prims = (mesh != NULL)
            ? cJSON_GetObjectItemCaseSensitive(mesh, "primitives") : NULL;
        int np = cJSON_IsArray(prims) ? cJSON_GetArraySize(prims) : 0;
        for (int i = 0; i < np; i++) {
            if (!add_primitive(g, bld, at(prims, i), world)) {
                return false;
            }
        }
    }

    const cJSON *kids = cJSON_GetObjectItemCaseSensitive(node, "children");
    int nk = cJSON_IsArray(kids) ? cJSON_GetArraySize(kids) : 0;
    for (int i = 0; i < nk; i++) {
        const cJSON *c = cJSON_GetArrayItem(kids, i);
        if (cJSON_IsNumber(c) && !walk_node(g, bld, (int)c->valuedouble, world, depth + 1)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */

bool vr_mesh_load_gltf(MeshWidget *m, size_t *vc, size_t *tc)
{
    size_t len = 0;
    char *file = read_whole_file(m->path, &len);
    if (file == NULL) {
        fprintf(stderr, "error: cannot open glTF '%s'.\n", m->path);
        return false;
    }

    const char    *json = file;
    unsigned char *bin = NULL;
    size_t         bin_len = 0;

    /*
     * GLB: a 12-byte header then length-prefixed chunks. The first is always
     * JSON; a binary chunk may follow. Both are read in place — the chunks are
     * slices of the buffer already in memory, so nothing is copied.
     */
    if (len >= 12 && memcmp(file, "glTF", 4) == 0) {
        size_t off = 12;
        char *jchunk = NULL;
        while (off + 8 <= len) {
            uint32_t clen, ctype;
            memcpy(&clen, file + off, 4);
            memcpy(&ctype, file + off + 4, 4);
            off += 8;
            if ((size_t)clen > len - off) {
                break;                       /* truncated */
            }
            if (ctype == 0x4E4F534AU && jchunk == NULL) {          /* 'JSON' */
                jchunk = file + off;
                /* The chunk is padded with spaces, not terminated; cJSON needs
                 * a NUL, and the byte after it is either the next chunk's
                 * length — which is re-read from the copy below — or the end. */
                jchunk = strndup(file + off, clen);
            } else if (ctype == 0x004E4942U && bin == NULL) {      /* 'BIN' */
                bin     = (unsigned char *)file + off;
                bin_len = clen;
            }
            off += clen;
            off = (off + 3u) & ~3u;          /* chunks are 4-byte aligned */
        }
        if (jchunk == NULL) {
            fprintf(stderr, "error: glTF '%s' — GLB has no JSON chunk.\n", m->path);
            free(file);
            return false;
        }
        json = jchunk;
    }

    Gltf g;
    memset(&g, 0, sizeof g);
    g.path = m->path;
    g.root = cJSON_Parse(json);

    if (json != file) {
        free((void *)json);
    }
    if (g.root == NULL) {
        fprintf(stderr, "error: glTF '%s' — invalid JSON.\n", m->path);
        free(file);
        return false;
    }

    g.views     = cJSON_GetObjectItemCaseSensitive(g.root, "bufferViews");
    g.accessors = cJSON_GetObjectItemCaseSensitive(g.root, "accessors");
    g.meshes    = cJSON_GetObjectItemCaseSensitive(g.root, "meshes");
    g.nodes     = cJSON_GetObjectItemCaseSensitive(g.root, "nodes");
    g.materials = cJSON_GetObjectItemCaseSensitive(g.root, "materials");
    g.textures  = cJSON_GetObjectItemCaseSensitive(g.root, "textures");
    g.images    = cJSON_GetObjectItemCaseSensitive(g.root, "images");

    if (!load_buffers(&g, bin, bin_len)) {
        gltf_release(&g);
        free(file);
        return false;
    }

    Build bld;
    memset(&bld, 0, sizeof bld);
    bld.m = m;
    bld.vc = vc;
    bld.tc = tc;
    bld.tex_source = -1;
    bld.ao_source = -1;
    bld.nrm_source = -1;
    bld.emis_source = -1;
    bld.normal_scale = 1.0f;

    /*
     * glTF is Y-up and +Z points at the viewer; this renderer's world is Y-down
     * with +Z going away. Without converting, every imported model arrives
     * upside down and inside out in depth — and because a lone model still
     * looks like a model, the mistake reads as "the artist built it that way".
     *
     * The conversion is a half turn about X: (x, -y, -z). Its determinant is
     * +1, so it is a rotation, not a mirror — triangle winding and therefore
     * backface culling survive it untouched.
     */
    float root[16];
    mat_identity(root);
    root[5] = -1.0f;
    root[10] = -1.0f;

    /*
     * The default scene, or every mesh in the file when there is no scene graph
     * at all — some exporters and many test files omit it, and importing
     * nothing from a file that plainly contains geometry is never what the
     * caller wanted.
     */
    const cJSON *scenes = cJSON_GetObjectItemCaseSensitive(g.root, "scenes");
    const cJSON *scene  = at(scenes, json_int_of(g.root, "scene", 0));
    const cJSON *roots  = (scene != NULL)
        ? cJSON_GetObjectItemCaseSensitive(scene, "nodes") : NULL;

    bool ok = true;
    if (cJSON_IsArray(roots)) {
        int n = cJSON_GetArraySize(roots);
        for (int i = 0; i < n && ok; i++) {
            const cJSON *c = cJSON_GetArrayItem(roots, i);
            if (cJSON_IsNumber(c)) {
                ok = walk_node(&g, &bld, (int)c->valuedouble, root, 0);
            }
        }
    } else {
        int n = cJSON_IsArray(g.meshes) ? cJSON_GetArraySize(g.meshes) : 0;
        for (int i = 0; i < n && ok; i++) {
            const cJSON *prims = cJSON_GetObjectItemCaseSensitive(at(g.meshes, i),
                                                                  "primitives");
            int np = cJSON_IsArray(prims) ? cJSON_GetArraySize(prims) : 0;
            for (int k = 0; k < np && ok; k++) {
                ok = add_primitive(&g, &bld, at(prims, k), root);
            }
        }
    }

    if (bld.skipped_modes > 0) {
        fprintf(stderr, "warning: glTF '%s' — skipped %d non-triangle primitive(s).\n",
                m->path, bld.skipped_modes);
    }
    if (cJSON_GetObjectItemCaseSensitive(g.root, "skins") != NULL ||
        cJSON_GetObjectItemCaseSensitive(g.root, "animations") != NULL) {
        fprintf(stderr, "note: glTF '%s' — animation and skinning are ignored; "
                        "the model is imported in its bind pose.\n", m->path);
    }

    /*
     * The base colour texture, unless the project named one. An image stored in
     * a bufferView rather than a file cannot go through the loader, which reads
     * paths, so that case is reported instead of silently dropping the texture.
     */
    if (ok && bld.tex_source >= 0 && m->tex_path == NULL) {
        const cJSON *img = at(g.images, bld.tex_source);
        const char *uri = (img != NULL) ? json_str_of(img, "uri") : NULL;

        if (uri != NULL && strncmp(uri, "data:", 5) != 0) {
            m->tex_path = sibling_path(m->path, uri);
            if (m->tex_path != NULL) {
                uri_unescape(m->tex_path);
            }
        } else if (img != NULL) {
            fprintf(stderr, "note: glTF '%s' — the base colour texture is embedded; "
                            "set \"texture\" on the object to supply it as a file.\n",
                    m->path);
        }
    }

    if (ok && bld.ao_source >= 0 && m->ao_path == NULL) {
        const cJSON *img = at(g.images, bld.ao_source);
        const char *uri = (img != NULL) ? json_str_of(img, "uri") : NULL;
        if (uri != NULL && strncmp(uri, "data:", 5) != 0) {
            m->ao_path = sibling_path(m->path, uri);
            if (m->ao_path != NULL) {
                uri_unescape(m->ao_path);
                if (bld.ao_packed) {
                    fprintf(stderr, "note: glTF '%s' — no occlusionTexture; reading "
                                    "occlusion from the red channel of '%s'.\n",
                            m->path, uri);
                }
            }
        }
    }

    if (ok && bld.nrm_source >= 0 && m->nrm_path == NULL) {
        const cJSON *img = at(g.images, bld.nrm_source);
        const char *uri = (img != NULL) ? json_str_of(img, "uri") : NULL;
        if (uri != NULL && strncmp(uri, "data:", 5) != 0) {
            m->nrm_path = sibling_path(m->path, uri);
            if (m->nrm_path != NULL) {
                uri_unescape(m->nrm_path);
                /* The project's own normal_scale, if it set one, wins over the
                 * material's — same rule as every other map here. */
                if (m->normal_scale < 0.0f) {
                    m->normal_scale = bld.normal_scale;
                }
            }
        }
    }

    if (ok && bld.emis_source >= 0 && m->emis_path == NULL) {
        const cJSON *img = at(g.images, bld.emis_source);
        const char *uri = (img != NULL) ? json_str_of(img, "uri") : NULL;
        if (uri != NULL && strncmp(uri, "data:", 5) != 0) {
            m->emis_path = sibling_path(m->path, uri);
            if (m->emis_path != NULL) {
                uri_unescape(m->emis_path);
            }
        }
    }
    if (ok && m->emissive_strength < 0.0f &&
        (bld.emissive[0] > 0.0f || bld.emissive[1] > 0.0f || bld.emissive[2] > 0.0f)) {
        m->emissive.r = byte_of(bld.emissive[0]);
        m->emissive.g = byte_of(bld.emissive[1]);
        m->emissive.b = byte_of(bld.emissive[2]);
        m->emissive.a = 255;
        m->emissive_strength = 1.0f;
    }

    /*
     * Tangents are all or nothing. If any primitive came without them the
     * partial array is thrown away here, and mesh_load derives the whole set
     * from the UVs — one consistent frame beats a correct one on part of the
     * model and zeroes on the rest.
     */
    if (bld.tan_missing && m->tans != NULL) {
        free(m->tans);
        m->tans = NULL;
    }

    gltf_release(&g);
    free(file);
    return ok && m->vert_count > 0 && m->tri_count > 0;
}
