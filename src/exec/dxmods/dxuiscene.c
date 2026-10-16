/***********************************************************************/
/* Open Visualization Data Explorer                                    */
/* DxuiScene: writes a renderable object as a scene file for the new   */
/* UI's GPU renderer. The format is defined in include/dxuiscene.h.    */
/***********************************************************************/

/*
 * The object is flattened into meshes of points, line segments and
 * triangles, each with its object-to-world matrix and DX shading values,
 * plus the lights DX would render with. The rules follow the renderers:
 *
 *   - shading values "ambient", "diffuse", "specular", "shininess" (or the
 *     "front ..." names) are inherited down the object tree, as the
 *     software renderer (libdx/lbshade.Z, parameters()) and the hardware
 *     renderer (hwrender/hwXfield.c, _dxf_parameters()) do; the defaults
 *     are theirs: 1, 0.7, 0.5, 10 (lbshade.Z default_parms,
 *     hwXfield.c defAttribute, libdx/xfield.c _dxf_XLighting()).
 *   - lights are gathered like lbshade.Z _GatherLights(): world distant
 *     lights are rotated by the enclosing xforms (translation dropped),
 *     camera lights stay camera relative, ambient lights add up, lights
 *     inside screen objects are ignored. Defaults as in _dxfGetLights():
 *     with no lights at all, one white distant light at camera direction
 *     (-1, 0, 1) (that is, eye + eye x up); with no ambient light, an
 *     ambient of 0.2 grey. The hardware renderer uses the same distant
 *     light (hwGather.c _dxf_gather()).
 *   - faces/loops/edges are triangulated with _dxfTriangulateField() (as
 *     both renderers do); invalid positions and invalid connections (and
 *     invalid polylines) remove the elements they touch.
 *   - groups of every kind are exported member by member. That includes
 *     series: given a whole series, both renderers draw all its members
 *     superimposed (libdx/gather.c _dxfGroup_Gather(), hwGather.c
 *     CLASS_SERIES); one frame at a time only comes from the Sequencer.
 */

#include <dxconfig.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dx/dx.h>
#include "_newtri.h"
#include "dxuiscene.h"

#define DEFAULT_AMBIENT    1.0f
#define DEFAULT_DIFFUSE    0.7f
#define DEFAULT_SPECULAR   0.5f
#define DEFAULT_SHININESS  10.0f
#define DEFAULT_AMBIENT_LIGHT 0.2f

#define DEP_NONE  0
#define DEP_POS   1
#define DEP_ELEM  2

typedef struct {
    float ambient, diffuse, specular, shininess;
    Matrix m;                       /* object to world, DX row-vector form */
    /* Inside a screen object: its position kind, depth flag and origin;
     * m then maps contents to pixels from the origin. */
    int screen;
    unsigned int screenPosition;
    int screenZ;
    float origin[3];
} State;

typedef struct {
    unsigned int kind, cameraRelative;
    float color[3], direction[3];
} SceneLight;

typedef struct {
    FILE *fp;
    char *path;
    unsigned int chunkCount;
    unsigned int skipped[DXS_SKIPPED_COUNT];
    float bounds[6];
    int haveBounds;
    SceneLight *lights;
    int nLights, maxLights;
    float ambient[3];
    int nAmbient;
    int littleEndian;
} Ctx;

/* --- writing ---------------------------------------------------------- */

static Error
writeWords(Ctx *c, const void *data, size_t nwords)
{
    if (nwords == 0)
        return OK;
    if (c->littleEndian) {
        if (fwrite(data, 4, nwords, c->fp) != nwords)
            goto error;
    } else {
        const unsigned char *p = (const unsigned char *)data;
        unsigned char b[4];
        size_t i;
        for (i = 0; i < nwords; i++, p += 4) {
            b[0] = p[3]; b[1] = p[2]; b[2] = p[1]; b[3] = p[0];
            if (fwrite(b, 4, 1, c->fp) != 1)
                goto error;
        }
    }
    return OK;
error:
    DXSetError(ERROR_INTERNAL, "DxuiScene: cannot write file %s", c->path);
    return ERROR;
}

static Error
writeU32(Ctx *c, unsigned int v)
{
    return writeWords(c, &v, 1);
}

/* --- small helpers ---------------------------------------------------- */

static void
addBounds(Ctx *c, const float *p, const Matrix *m)
{
    float w[3];
    int i;
    for (i = 0; i < 3; i++)
        w[i] = p[0]*m->A[0][i] + p[1]*m->A[1][i] + p[2]*m->A[2][i] + m->b[i];
    if (!isfinite(w[0]) || !isfinite(w[1]) || !isfinite(w[2]))
        return;
    if (!c->haveBounds) {
        for (i = 0; i < 3; i++)
            c->bounds[i] = c->bounds[i+3] = w[i];
        c->haveBounds = 1;
    } else {
        for (i = 0; i < 3; i++) {
            if (w[i] < c->bounds[i])   c->bounds[i] = w[i];
            if (w[i] > c->bounds[i+3]) c->bounds[i+3] = w[i];
        }
    }
}

static Error
shadingValue(Object o, char *name, char *frontName, float *value)
{
    Object a = DXGetAttribute(o, name);
    if (!a)
        a = DXGetAttribute(o, frontName);
    if (!a)
        return OK;
    if (!DXExtractFloat(a, value)) {
        DXSetError(ERROR_BAD_PARAMETER, "%s attribute must be a float", name);
        return ERROR;
    }
    return OK;
}

static Error
inheritParameters(Object o, State *st)
{
    if (!shadingValue(o, "ambient", "front ambient", &st->ambient) ||
        !shadingValue(o, "diffuse", "front diffuse", &st->diffuse) ||
        !shadingValue(o, "specular", "front specular", &st->specular) ||
        !shadingValue(o, "shininess", "front shininess", &st->shininess))
        return ERROR;
    return OK;
}

static Error
addLight(Ctx *c, unsigned int kind, unsigned int rel, RGBColor col, Vector d)
{
    SceneLight *l;
    if (c->nLights == c->maxLights) {
        int n = c->maxLights ? 2 * c->maxLights : 8;
        SceneLight *nl = (SceneLight *)DXReAllocate((Pointer)c->lights,
                                                    n * sizeof(SceneLight));
        if (!nl)
            return ERROR;
        c->lights = nl;
        c->maxLights = n;
    }
    l = &c->lights[c->nLights++];
    l->kind = kind;
    l->cameraRelative = rel;
    l->color[0] = col.r; l->color[1] = col.g; l->color[2] = col.b;
    l->direction[0] = d.x; l->direction[1] = d.y; l->direction[2] = d.z;
    return OK;
}

static Error
gatherLight(Ctx *c, Light l, const State *st)
{
    Vector d;
    RGBColor col;

    if (st->screen)                 /* DX ignores lights in screen objects */
        return OK;

    if (DXQueryAmbientLight(l, &col)) {
        c->ambient[0] += col.r;
        c->ambient[1] += col.g;
        c->ambient[2] += col.b;
        c->nAmbient++;
    } else if (DXQueryDistantLight(l, &d, &col)) {
        Matrix m = st->m;
        float len;
        m.b[0] = m.b[1] = m.b[2] = 0;
        d = DXApply(d, m);
        len = DXLength(d);
        if (!(len > 0) || !isfinite(len)) {
            c->skipped[DXS_SKIPPED_OTHER]++;
            return OK;
        }
        d = DXDiv(d, len);
        return addLight(c, DXS_LIGHT_DISTANT, 0, col, d);
    } else if (DXQueryCameraDistantLight(l, &d, &col)) {
        return addLight(c, DXS_LIGHT_DISTANT, 1, col, d);
    } else {
        /* point lights: neither DX renderer draws them */
        c->skipped[DXS_SKIPPED_OTHER]++;
    }
    return OK;
}

/*
 * Reads a per-vertex or per-element component into a newly allocated float
 * array of n * ncomp values. Returns DEP_NONE (and no array) when the
 * component is absent or unusable; unusable ones are reported as warnings,
 * like a renderer that draws what it can.
 */
static int
readComponent(Field f, char *name, char *mapName, int ncomp,
              int npos, int nelem, float **out)
{
    Array a, map = NULL, conv = NULL;
    int n, rank, shape[32], comps, dep, i, j, nmap = 0;
    Type t;
    Category cat;
    char *depName = NULL;
    float *res = NULL;
    Pointer data;

    *out = NULL;
    a = (Array)DXGetComponentValue(f, name);
    if (!a)
        return DEP_NONE;
    if (DXGetObjectClass((Object)a) != CLASS_ARRAY ||
        !DXGetArrayInfo(a, &n, &t, &cat, &rank, shape))
        goto unusable;
    if (rank == 0)
        comps = 1;
    else if (rank == 1 && shape[0] >= 1)
        comps = shape[0];
    else
        goto unusable;
    if (cat != CATEGORY_REAL)
        goto unusable;

    if (!DXGetStringAttribute((Object)a, "dep", &depName)) {
        DXResetError();
        depName = NULL;
    }
    if (depName && !strcmp(depName, "positions"))
        dep = DEP_POS;
    else if (depName && (!strcmp(depName, "connections") ||
                         !strcmp(depName, "polylines")))
        dep = DEP_ELEM;
    else if (!depName && n == npos)
        dep = DEP_POS;
    else
        goto unusable;
    if ((dep == DEP_POS && n != npos) || (dep == DEP_ELEM && n != nelem))
        goto unusable;
    if (n <= 0)
        return DEP_NONE;

    res = (float *)DXAllocate((size_t)n * ncomp * sizeof(float));
    if (!res)
        return -1;

    if (t == TYPE_UBYTE && comps == 1 && mapName &&
        (map = (Array)DXGetComponentValue(f, mapName)) != NULL) {
        /* indexed colours/opacities: look up the map */
        int mrank, mshape[32], mcomps;
        Type mt;
        Category mc;
        float *md;
        unsigned char *idx;
        Array mconv = NULL;

        if (DXGetObjectClass((Object)map) != CLASS_ARRAY ||
            !DXGetArrayInfo(map, &nmap, &mt, &mc, &mrank, mshape))
            goto unusable;
        mcomps = mrank == 0 ? 1 : (mrank == 1 ? mshape[0] : 0);
        if (mcomps != ncomp || nmap <= 0)
            goto unusable;
        if (mt != TYPE_FLOAT) {
            mconv = mrank == 0
                ? DXArrayConvert(map, TYPE_FLOAT, CATEGORY_REAL, 0)
                : DXArrayConvert(map, TYPE_FLOAT, CATEGORY_REAL, 1, mcomps);
            if (!mconv)
                goto unusable;
            md = (float *)DXGetArrayData(mconv);
        } else
            md = (float *)DXGetArrayData(map);
        idx = (unsigned char *)DXGetArrayData(a);
        if (!md || !idx) {
            DXDelete((Object)mconv);
            goto unusable;
        }
        for (i = 0; i < n; i++) {
            int k = idx[i] < nmap ? idx[i] : nmap - 1;
            for (j = 0; j < ncomp; j++)
                res[i*ncomp + j] = md[k*ncomp + j];
        }
        DXDelete((Object)mconv);
        *out = res;
        return dep;
    }

    if (t == TYPE_FLOAT) {
        data = DXGetArrayData(a);
    } else {
        conv = rank == 0
            ? DXArrayConvert(a, TYPE_FLOAT, CATEGORY_REAL, 0)
            : DXArrayConvert(a, TYPE_FLOAT, CATEGORY_REAL, 1, comps);
        if (!conv)
            goto unusable;
        data = DXGetArrayData(conv);
    }
    if (!data)
        goto unusable;
    {
        float *src = (float *)data;
        /* ubyte RGB colours are 0..255 */
        float scale = (t == TYPE_UBYTE && comps == 3 && mapName) ?
                      1.0f / 255.0f : 1.0f;
        for (i = 0; i < n; i++)
            for (j = 0; j < ncomp; j++)
                res[i*ncomp + j] = j < comps ? src[i*comps + j] * scale : 0.0f;
    }
    DXDelete((Object)conv);
    *out = res;
    return dep;

unusable:
    DXResetError();
    DXDelete((Object)conv);
    DXFree((Pointer)res);
    DXWarning("DxuiScene: ignoring unusable \"%s\" component", name);
    return DEP_NONE;
}

/* --- fields ----------------------------------------------------------- */

static Error exportField(Ctx *c, Field f, const State *st);

/* Emits one mesh. pv/pe: vertex and element index of each output vertex. */
static Error
writeMesh(Ctx *c, const State *st, unsigned int primitive,
          const float *pos, int npos, const int *pv, const int *pe, int nout,
          const float *nrm, int ndep, const float *col, int cdep,
          const float *opa, int odep)
{
    int indexed, nv, ni, i, k;
    unsigned int flags = 0, header[4];
    double bytes;
    float matrix[16], shading[4];
    float *buf = NULL;
    Error ret = ERROR;

    if (nout <= 0)
        return OK;

    /* Elements' own values force one vertex per element corner. */
    indexed = !(ndep == DEP_ELEM || cdep == DEP_ELEM || odep == DEP_ELEM);
    if (primitive == DXS_POINTS)
        indexed = 0;
    nv = indexed ? npos : nout;
    ni = indexed ? nout : 0;

    if (nrm) flags |= DXS_HAS_NORMALS;
    if (col) flags |= DXS_HAS_COLORS;
    if (opa) flags |= DXS_HAS_OPACITIES;

    bytes = 4.0 * (4 + 16 + 4) + 12.0 * nv + 4.0 * ni;
    if (st->screen)
        bytes += 4.0 * 5;           /* position, z, origin */
    if (nrm) bytes += 12.0 * nv;
    if (col) bytes += 12.0 * nv;
    if (opa) bytes += 4.0 * nv;
    if (bytes > 4294967295.0) {
        DXWarning("DxuiScene: mesh too large for one chunk, skipped");
        c->skipped[DXS_SKIPPED_OTHER]++;
        return OK;
    }

    for (k = 0; k < 3; k++) {
        matrix[k*4 + 0] = st->m.A[k][0];
        matrix[k*4 + 1] = st->m.A[k][1];
        matrix[k*4 + 2] = st->m.A[k][2];
        matrix[k*4 + 3] = 0.0f;
    }
    matrix[12] = st->m.b[0];
    matrix[13] = st->m.b[1];
    matrix[14] = st->m.b[2];
    matrix[15] = 1.0f;
    shading[0] = st->ambient;
    shading[1] = st->diffuse;
    shading[2] = st->specular;
    shading[3] = st->shininess;

    header[0] = primitive;
    header[1] = (unsigned int)nv;
    header[2] = (unsigned int)ni;
    header[3] = flags;

    if (st->screen) {
        unsigned int z = (unsigned int)st->screenZ;     /* int32 on disk */
        if (!writeU32(c, DXS_CHUNK_SCREEN) || !writeU32(c, (unsigned int)bytes) ||
            !writeU32(c, st->screenPosition) || !writeU32(c, z) ||
            !writeWords(c, st->origin, 3))
            goto done;
    } else if (!writeU32(c, DXS_CHUNK_MESH) || !writeU32(c, (unsigned int)bytes))
        goto done;
    if (!writeWords(c, header, 4) || !writeWords(c, matrix, 16) ||
        !writeWords(c, shading, 4))
        goto done;

    if (indexed) {
        if (!writeWords(c, pos, 3 * (size_t)npos))
            goto done;
        if (nrm && !writeWords(c, nrm, 3 * (size_t)npos)) goto done;
        if (col && !writeWords(c, col, 3 * (size_t)npos)) goto done;
        if (opa && !writeWords(c, opa, (size_t)npos)) goto done;
        if (!writeWords(c, pv, (size_t)nout))
            goto done;
        for (i = 0; i < nout && !st->screen; i++)
            addBounds(c, pos + 3*pv[i], &st->m);
    } else {
        buf = (float *)DXAllocate((size_t)nv * 3 * sizeof(float));
        if (!buf)
            goto done;
#define EXPAND(src, dep, nc) {                                          \
        for (i = 0; i < nout; i++) {                                    \
            int s_ = (dep) == DEP_ELEM ? pe[i] : pv[i];                 \
            for (k = 0; k < (nc); k++)                                  \
                buf[i*(nc) + k] = (src)[s_*(nc) + k];                   \
        }                                                               \
        if (!writeWords(c, buf, (size_t)nout * (nc))) goto done;        \
    }
        EXPAND(pos, DEP_POS, 3);
        if (nrm) EXPAND(nrm, ndep, 3);
        if (col) EXPAND(col, cdep, 3);
        if (opa) EXPAND(opa, odep, 1);
#undef EXPAND
        for (i = 0; i < nout && !st->screen; i++)
            addBounds(c, pos + 3*pv[i], &st->m);
    }
    c->chunkCount++;
    ret = OK;

done:
    DXFree((Pointer)buf);
    return ret;
}

static Error
triangulatedField(Ctx *c, Field f, const State *st)
{
    Field copy = (Field)DXCopy((Object)f, COPY_HEADER);
    Error ret = OK;

    if (!copy)
        return ERROR;
    DXReference((Object)copy);
    if (!_dxfTriangulateField(copy) ||
        !DXGetComponentValue(copy, "connections")) {
        /* could not triangulate (or nothing left): not exported */
        DXResetError();
        c->skipped[DXS_SKIPPED_OTHER]++;
    } else
        ret = exportField(c, copy, st);
    DXDelete((Object)copy);
    return ret;
}

static Error
exportField(Ctx *c, Field f, const State *st)
{
    Array pa, ca = NULL, ea = NULL, conv = NULL;
    int npos, rank, shape[32], dim, i, j, k;
    int nelem = 0, nper = 0, nedges = 0;
    unsigned int primitive;
    Type t;
    Category cat;
    char *etype = NULL;
    const int *conn = NULL, *edges = NULL;
    float *pos = NULL, *nrm = NULL, *col = NULL, *opa = NULL;
    const float *src;
    int ndep, cdep, odep;
    int *pv = NULL, *pe = NULL, nout = 0, maxout;
    InvalidComponentHandle ipos = NULL, ielem = NULL;
    Error ret = ERROR;

    if (DXEmptyField(f))
        return OK;

    if (!DXGetComponentValue(f, "connections") &&
        DXGetComponentValue(f, "faces"))
        return triangulatedField(c, f, st);

    pa = (Array)DXGetComponentValue(f, "positions");
    if (!pa || DXGetObjectClass((Object)pa) != CLASS_ARRAY ||
        !DXGetArrayInfo(pa, &npos, &t, &cat, &rank, shape) ||
        npos <= 0 || cat != CATEGORY_REAL) {
        DXResetError();
        c->skipped[DXS_SKIPPED_OTHER]++;
        return OK;
    }
    dim = rank == 0 ? 1 : (rank == 1 ? shape[0] : 0);
    if (dim < 1 || dim > 3) {
        c->skipped[DXS_SKIPPED_OTHER]++;
        return OK;
    }

    /* connections */
    if ((ca = (Array)DXGetComponentValue(f, "connections")) != NULL) {
        int crank, cshape[32];
        Type ct;
        if (!DXGetStringAttribute((Object)ca, "element type", &etype) ||
            !etype) {
            DXResetError();
            c->skipped[DXS_SKIPPED_OTHER]++;
            return OK;
        }
        if (!strcmp(etype, "cubes") || !strcmp(etype, "tetrahedra")) {
            c->skipped[DXS_SKIPPED_VOLUME]++;
            return OK;
        }
        if (!strcmp(etype, "triangles"))
            primitive = DXS_TRIANGLES, nper = 3;
        else if (!strcmp(etype, "quads"))
            primitive = DXS_TRIANGLES, nper = 4;
        else if (!strcmp(etype, "lines"))
            primitive = DXS_LINES, nper = 2;
        else {
            c->skipped[DXS_SKIPPED_OTHER]++;
            return OK;
        }
        if (DXGetObjectClass((Object)ca) != CLASS_ARRAY ||
            !DXGetArrayInfo(ca, &nelem, &ct, NULL, &crank, cshape) ||
            ct != TYPE_INT || crank != 1 || cshape[0] != nper) {
            DXResetError();
            c->skipped[DXS_SKIPPED_OTHER]++;
            return OK;
        }
        if (nelem > 0 && !(conn = (const int *)DXGetArrayData(ca)))
            goto done;
    } else if ((ca = (Array)DXGetComponentValue(f, "polylines")) != NULL) {
        Type pt, et;
        int prank, erank, eshape[32];
        ea = (Array)DXGetComponentValue(f, "edges");
        if (!ea || DXGetObjectClass((Object)ca) != CLASS_ARRAY ||
            DXGetObjectClass((Object)ea) != CLASS_ARRAY ||
            !DXGetArrayInfo(ca, &nelem, &pt, NULL, &prank, NULL) ||
            !DXGetArrayInfo(ea, &nedges, &et, NULL, &erank, eshape) ||
            pt != TYPE_INT || et != TYPE_INT ||
            (erank != 0 && !(erank == 1 && eshape[0] == 1))) {
            DXResetError();
            c->skipped[DXS_SKIPPED_OTHER]++;
            return OK;
        }
        primitive = DXS_LINES;
        if (nelem > 0 && !(conn = (const int *)DXGetArrayData(ca)))
            goto done;
        if (nedges > 0 && !(edges = (const int *)DXGetArrayData(ea)))
            goto done;
    } else {
        primitive = DXS_POINTS;
        nelem = npos;
    }

    if (DXGetAttribute((Object)f, "texture"))
        c->skipped[DXS_SKIPPED_TEXTURE]++;

    /* positions, padded to 3D */
    if (t == TYPE_FLOAT)
        src = (const float *)DXGetArrayData(pa);
    else {
        conv = rank == 0 ? DXArrayConvert(pa, TYPE_FLOAT, CATEGORY_REAL, 0)
                         : DXArrayConvert(pa, TYPE_FLOAT, CATEGORY_REAL, 1, dim);
        if (!conv) {
            DXResetError();
            c->skipped[DXS_SKIPPED_OTHER]++;
            ret = OK;
            goto done;
        }
        src = (const float *)DXGetArrayData(conv);
    }
    if (!src)
        goto done;
    pos = (float *)DXAllocate((size_t)npos * 3 * sizeof(float));
    if (!pos)
        goto done;
    for (i = 0; i < npos; i++)
        for (k = 0; k < 3; k++)
            pos[3*i + k] = k < dim ? src[i*dim + k] : 0.0f;
    DXDelete((Object)conv);
    conv = NULL;

    /* invalid positions and elements */
    if (DXGetComponentValue(f, "invalid positions")) {
        ipos = DXCreateInvalidComponentHandle((Object)f, NULL, "positions");
        if (!ipos)
            goto done;
    }
    if (primitive != DXS_POINTS) {
        char *iname = ea ? "invalid polylines" : "invalid connections";
        if (DXGetComponentValue(f, iname)) {
            ielem = DXCreateInvalidComponentHandle((Object)f, NULL,
                                       ea ? "polylines" : "connections");
            if (!ielem)
                goto done;
        }
    }

    /* the per-vertex / per-element values */
    ndep = readComponent(f, "normals", NULL, 3, npos, nelem, &nrm);
    if (DXGetComponentValue(f, "colors"))
        cdep = readComponent(f, "colors", "color map", 3, npos, nelem, &col);
    else if (DXGetComponentValue(f, "front colors"))
        cdep = readComponent(f, "front colors", "color map", 3,
                             npos, nelem, &col);
    else
        cdep = readComponent(f, "back colors", "color map", 3,
                             npos, nelem, &col);
    odep = readComponent(f, "opacities", "opacity map", 1, npos, nelem, &opa);
    if (ndep < 0 || cdep < 0 || odep < 0)
        goto done;
    if (primitive == DXS_POINTS) {
        /* only position-dependent values make sense on points */
        if (ndep == DEP_ELEM && nelem != npos) { DXFree(nrm); nrm = NULL; }
        if (cdep == DEP_ELEM && nelem != npos) { DXFree(col); col = NULL; }
        if (odep == DEP_ELEM && nelem != npos) { DXFree(opa); opa = NULL; }
    }

    /* the output vertices: (vertex, element) per primitive corner */
    if (primitive == DXS_POINTS)
        maxout = npos;
    else if (ea)
        maxout = nedges > 0 ? 2 * nedges : 0;
    else
        maxout = nelem * (nper == 4 ? 6 : nper);
    if (maxout <= 0) {
        ret = OK;
        goto done;
    }
    pv = (int *)DXAllocate((size_t)maxout * sizeof(int));
    pe = (int *)DXAllocate((size_t)maxout * sizeof(int));
    if (!pv || !pe)
        goto done;

#define BADV(v) ((v) < 0 || (v) >= npos || (ipos && DXIsElementInvalid(ipos, (v))))
    if (primitive == DXS_POINTS) {
        for (i = 0; i < npos; i++)
            if (!BADV(i)) {
                pv[nout] = i; pe[nout] = i; nout++;
            }
    } else if (ea) {
        for (i = 0; i < nelem; i++) {
            int start = conn[i];
            int end = i + 1 < nelem ? conn[i+1] : nedges;
            if (ielem && DXIsElementInvalid(ielem, i))
                continue;
            if (start < 0 || end > nedges || start >= end)
                continue;
            for (j = start; j + 1 < end; j++) {
                int a = edges[j], b = edges[j+1];
                if (BADV(a) || BADV(b))
                    continue;
                pv[nout] = a; pe[nout] = i; nout++;
                pv[nout] = b; pe[nout] = i; nout++;
            }
        }
    } else {
        static const int quadTris[6] = { 0, 3, 1, 0, 2, 3 };
        for (i = 0; i < nelem; i++) {
            const int *e = conn + (size_t)i * nper;
            int bad = ielem && DXIsElementInvalid(ielem, i);
            for (k = 0; k < nper && !bad; k++)
                bad = BADV(e[k]);
            if (bad)
                continue;
            if (nper == 4) {
                for (k = 0; k < 6; k++) {
                    pv[nout] = e[quadTris[k]]; pe[nout] = i; nout++;
                }
            } else {
                for (k = 0; k < nper; k++) {
                    pv[nout] = e[k]; pe[nout] = i; nout++;
                }
            }
        }
    }
#undef BADV

    ret = writeMesh(c, st, primitive, pos, npos, pv, pe, nout,
                    nrm, ndep, col, cdep, opa, odep);

done:
    if (ipos)  DXFreeInvalidComponentHandle(ipos);
    if (ielem) DXFreeInvalidComponentHandle(ielem);
    DXDelete((Object)conv);
    DXFree((Pointer)pos);
    DXFree((Pointer)nrm);
    DXFree((Pointer)col);
    DXFree((Pointer)opa);
    DXFree((Pointer)pv);
    DXFree((Pointer)pe);
    return ret;
}

/* --- traversal -------------------------------------------------------- */

static Error
traverse(Ctx *c, Object o, State st)
{
    Class cl;

    if (!o)
        return OK;
    if (!inheritParameters(o, &st))
        return ERROR;

    cl = DXGetObjectClass(o);
    switch (cl) {
    case CLASS_FIELD:
        return exportField(c, (Field)o, &st);

    case CLASS_GROUP: {
        Object child;
        int i;
        /* every member, series included: both renderers draw them all */
        for (i = 0; (child = DXGetEnumeratedMember((Group)o, i, NULL)); i++)
            if (!traverse(c, child, st))
                return ERROR;
        return OK;
    }

    case CLASS_XFORM: {
        Object child;
        Matrix m;
        if (!DXGetXformInfo((Xform)o, &child, &m))
            return ERROR;
        st.m = DXConcatenate(m, st.m);
        return traverse(c, child, st);
    }

    case CLASS_SCREEN: {
        /* As _dxfScreen_Shade (lbshade.Z):
         * - stationary: the origin resets to zero but the camera still
         *   applies, so this only drops the enclosing transforms;
         * - otherwise, at the first screen object: its contents become
         *   pixels from an origin, the translation of the transforms so far
         *   (a viewport fraction, pixels, or a world point);
         * - a screen object inside one: rotation and scale are dropped, the
         *   origin stays.
         * Captions are a stationary screen, a translation, then a viewport
         * screen around the text. */
        Object child;
        int fixed, z;
        if (!DXGetScreenInfo((Screen)o, &child, &fixed, &z))
            return ERROR;
        if (fixed == SCREEN_STATIONARY) {
            memset(&st.m, 0, sizeof(st.m));
            st.m.A[0][0] = st.m.A[1][1] = st.m.A[2][2] = 1.0f;
        } else if (!st.screen) {
            st.screen = 1;
            st.screenPosition = (unsigned int)fixed;
            st.screenZ = z > 0 ? 1 : (z < 0 ? -1 : 0);
            st.origin[0] = st.m.b[0];
            st.origin[1] = st.m.b[1];
            st.origin[2] = st.m.b[2];
            memset(&st.m, 0, sizeof(st.m));
            st.m.A[0][0] = st.m.A[1][1] = st.m.A[2][2] = 1.0f;
        } else {
            memset(&st.m.A, 0, sizeof(st.m.A));
            st.m.A[0][0] = st.m.A[1][1] = st.m.A[2][2] = 1.0f;
        }
        return traverse(c, child, st);
    }

    case CLASS_CLIPPED: {
        Object render = NULL, clipping = NULL;
        if (!DXGetClippedInfo((Clipped)o, &render, &clipping))
            return ERROR;
        c->skipped[DXS_SKIPPED_CLIPPED]++;
        return traverse(c, render, st);
    }

    case CLASS_LIGHT:
        return gatherLight(c, (Light)o, &st);

    default:
        c->skipped[DXS_SKIPPED_OTHER]++;
        return OK;
    }
}

static Error
writeLights(Ctx *c)
{
    int i, nDistant = 0;
    unsigned int words[2 + 6];
    float *fw = (float *)(words + 2);

    for (i = 0; i < c->nLights; i++)
        if (c->lights[i].kind == DXS_LIGHT_DISTANT)
            nDistant++;

    /* defaults, as libdx/lbshade.Z _dxfGetLights() */
    if (nDistant == 0 && c->nAmbient == 0) {
        Vector d = DXVec(-1.0, 0.0, 1.0);
        d = DXNormalize(d);
        if (!addLight(c, DXS_LIGHT_DISTANT, 1, DXRGB(1.0, 1.0, 1.0), d))
            return ERROR;
    }
    if (c->nAmbient == 0)
        c->ambient[0] = c->ambient[1] = c->ambient[2] = DEFAULT_AMBIENT_LIGHT;

    /* one ambient light: DX sums them */
    words[0] = DXS_LIGHT_AMBIENT;
    words[1] = 0;
    fw[0] = c->ambient[0]; fw[1] = c->ambient[1]; fw[2] = c->ambient[2];
    fw[3] = fw[4] = fw[5] = 0.0f;
    if (!writeU32(c, DXS_CHUNK_LIGHT) || !writeU32(c, sizeof(words)) ||
        !writeWords(c, words, 8))
        return ERROR;
    c->chunkCount++;

    for (i = 0; i < c->nLights; i++) {
        SceneLight *l = &c->lights[i];
        words[0] = l->kind;
        words[1] = l->cameraRelative;
        memcpy(fw, l->color, sizeof(l->color));
        memcpy(fw + 3, l->direction, sizeof(l->direction));
        if (!writeU32(c, DXS_CHUNK_LIGHT) || !writeU32(c, sizeof(words)) ||
            !writeWords(c, words, 8))
            return ERROR;
        c->chunkCount++;
    }
    return OK;
}

static Error
writeInfo(Ctx *c)
{
    float b[6];
    if (c->haveBounds)
        memcpy(b, c->bounds, sizeof(b));
    else
        memset(b, 0, sizeof(b));
    if (!writeU32(c, DXS_CHUNK_INFO) ||
        !writeU32(c, 4 * (DXS_SKIPPED_COUNT + 6)) ||
        !writeWords(c, c->skipped, DXS_SKIPPED_COUNT) ||
        !writeWords(c, b, 6))
        return ERROR;
    c->chunkCount++;
    return OK;
}

Error
m_DxuiScene(Object *in, Object *out)
{
    Ctx c;
    State st;
    char *name = NULL, *tmp = NULL;
    unsigned int one = 1;
    Error ret = ERROR;

    memset(&c, 0, sizeof(c));
    c.littleEndian = *(unsigned char *)&one == 1;

    if (!in[1] || !DXExtractString(in[1], &name) || !name || !*name) {
        DXSetError(ERROR_BAD_PARAMETER, "name must be a file name string");
        return ERROR;
    }
    c.path = name;

    tmp = (char *)DXAllocate(strlen(name) + 5);
    if (!tmp)
        return ERROR;
    strcpy(tmp, name);
    strcat(tmp, ".tmp");

    c.fp = fopen(tmp, "wb");
    if (!c.fp) {
        DXSetError(ERROR_BAD_PARAMETER, "DxuiScene: cannot open file %s",
                   tmp);
        DXFree((Pointer)tmp);
        return ERROR;
    }

    memset(&st, 0, sizeof(st));      /* not in a screen object */
    st.ambient = DEFAULT_AMBIENT;
    st.diffuse = DEFAULT_DIFFUSE;
    st.specular = DEFAULT_SPECULAR;
    st.shininess = DEFAULT_SHININESS;
    memset(&st.m, 0, sizeof(st.m));
    st.m.A[0][0] = st.m.A[1][1] = st.m.A[2][2] = 1.0f;

    if (fwrite(DXS_MAGIC, 1, 8, c.fp) != 8 || !writeU32(&c, 0)) {
        DXSetError(ERROR_INTERNAL, "DxuiScene: cannot write file %s", tmp);
        goto done;
    }
    if (!traverse(&c, in[0], st))
        goto done;
    if (!writeLights(&c) || !writeInfo(&c))
        goto done;
    if (fseek(c.fp, 8, SEEK_SET) != 0 || !writeU32(&c, c.chunkCount)) {
        DXSetError(ERROR_INTERNAL, "DxuiScene: cannot write file %s", tmp);
        goto done;
    }
    if (fclose(c.fp) != 0) {
        c.fp = NULL;
        DXSetError(ERROR_INTERNAL, "DxuiScene: cannot write file %s", tmp);
        goto done;
    }
    c.fp = NULL;
    if (rename(tmp, name) != 0) {
        DXSetError(ERROR_INTERNAL, "DxuiScene: cannot rename %s to %s",
                   tmp, name);
        goto done;
    }
    ret = OK;

done:
    if (c.fp)
        fclose(c.fp);
    if (ret != OK)
        remove(tmp);
    DXFree((Pointer)tmp);
    DXFree((Pointer)c.lights);
    return ret;
}
