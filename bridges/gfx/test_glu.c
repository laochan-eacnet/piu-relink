#include <windows.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glu_callbacks.h"
#include "gl_generated.h"
#include "../../lxrt/lxabi.h"

_Static_assert(sizeof(void *) == 4, "test_glu requires an i686 executable");

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "FAIL: %s (line %d; thread=%lu; Win32=%lu)\n", \
            #expression, __LINE__, GetCurrentThreadId(), GetLastError()); \
    exit(3); \
} } while (0)

#define GLU_APIS(X) \
    X(gluNewTess) X(gluDeleteTess) X(gluTessCallback) \
    X(gluTessBeginPolygon) X(gluTessBeginContour) X(gluTessVertex) \
    X(gluTessEndContour) X(gluTessEndPolygon) X(gluTessProperty) \
    X(gluTessNormal) X(gluGetTessProperty) \
    X(gluNewQuadric) X(gluDeleteQuadric) X(gluQuadricCallback) \
    X(gluQuadricNormals) X(gluQuadricOrientation) \
    X(gluNewNurbsRenderer) X(gluDeleteNurbsRenderer) X(gluNurbsCallback) \
    X(gluNurbsProperty) X(gluErrorString)

static struct {
#define API_FIELD(name) __typeof__(&lx_##name) name;
    GLU_APIS(API_FIELD)
#undef API_FIELD
} api;

static const lx_sym *symbols;
static unsigned symbol_count;

static void *find_symbol(const char *name)
{
    for (unsigned i = 0; i < symbol_count; ++i)
        if (!strcmp(symbols[i].name, name)) return symbols[i].addr;
    fprintf(stderr, "missing provider symbol: %s\n", name);
    exit(2);
}

enum { REGULAR, DATA, NO_CALLBACK };
enum { BEGIN, VERTEX, END, EDGE, ERROR_CALLBACK, COMBINE, CALLBACK_COUNT };
enum polygon_kind { BOWTIE, WITHOUT_COMBINE, SQUARE };

static const GLenum selectors[2][CALLBACK_COUNT] = {
    {GLU_TESS_BEGIN, GLU_TESS_VERTEX, GLU_TESS_END, GLU_TESS_EDGE_FLAG,
     GLU_TESS_ERROR, GLU_TESS_COMBINE},
    {GLU_TESS_BEGIN_DATA, GLU_TESS_VERTEX_DATA, GLU_TESS_END_DATA,
     GLU_TESS_EDGE_FLAG_DATA, GLU_TESS_ERROR_DATA, GLU_TESS_COMBINE_DATA}
};

typedef struct tess_case tess_case;
typedef struct {
    tess_case *owner;
    unsigned serial;
    GLdouble coords[3];
    int generated;
} vertex;

typedef struct {
    HANDLE entered[2];
    HANDLE progress;
} thread_sync;

struct tess_case {
    GLUtesselator *tess;
    unsigned slot;
    DWORD owner_thread;
    unsigned serial;
    struct { tess_case *owner; unsigned serial; } polygon;
    vertex input[4];
    vertex generated[16];
    unsigned generated_count;
    unsigned generated_seen;
    unsigned expected_modes[CALLBACK_COUNT];
    GLenum expected_error;
    unsigned counts[2][CALLBACK_COUNT];
    unsigned original_vertices[2];
    unsigned replacement_vertices[2];
    unsigned polygons;
    unsigned stack_checks;
    unsigned primitive_vertices;
    GLenum primitive_type;
    int in_primitive;
    tess_case *nested;
    unsigned nested_runs;
    thread_sync *sync;
    unsigned thread_index;
    int rendezvous_done;
};

/* This is the test's independent expected-object stack, not bridge state. */
static __declspec(thread) tess_case *active_case;

static void tessellate(tess_case *s, unsigned mode, unsigned vertex_mode,
                      enum polygon_kind polygon);

static tess_case *callback_case(unsigned slot, unsigned mode, unsigned kind,
                                void *polygon_data)
{
    tess_case *s = active_case;
    CHECK(s != NULL);
    CHECK(s->owner_thread == GetCurrentThreadId());
    CHECK(s->slot == slot);
    CHECK(s->expected_modes[kind] == mode);
    if (mode == DATA) {
        CHECK(polygon_data == &s->polygon);
        CHECK(s->polygon.owner == s && s->polygon.serial == s->serial);
    }
    return s;
}

static int close_enough(GLdouble a, GLdouble b)
{
    GLdouble difference = a - b;
    return difference > -0.00001 && difference < 0.00001;
}

static vertex *checked_vertex(tess_case *s, void *data)
{
    vertex *v = NULL;
    for (unsigned i = 0; i < 4; ++i)
        if (data == &s->input[i]) v = &s->input[i];
    for (unsigned i = 0; i < s->generated_count; ++i)
        if (data == &s->generated[i]) v = &s->generated[i];
    CHECK(v != NULL);
    CHECK(v->owner == s && v->serial == s->serial);
    return v;
}

static void on_begin(unsigned slot, unsigned mode, GLenum primitive, void *polygon_data)
{
    tess_case *s = callback_case(slot, mode, BEGIN, polygon_data);
    CHECK(!s->in_primitive);
    CHECK(primitive == GL_TRIANGLES || primitive == GL_TRIANGLE_STRIP ||
          primitive == GL_TRIANGLE_FAN);
    s->in_primitive = 1;
    s->primitive_type = primitive;
    s->primitive_vertices = 0;
    ++s->counts[mode][BEGIN];
    if (s->nested && !s->nested_runs) {
        ++s->nested_runs;
        tessellate(s->nested, DATA, DATA, BOWTIE);
        CHECK(active_case == s);
    }
    if (s->sync && !s->rendezvous_done) {
        s->rendezvous_done = 1;
        CHECK(SetEvent(s->sync->entered[s->thread_index]));
        CHECK(WaitForSingleObject(s->sync->entered[1 - s->thread_index], 15000) == WAIT_OBJECT_0);
        /* Hold thread 1 inside GLU while thread 0 receives another callback.
           A process-global bridge active-object pointer cannot pass this. */
        if (s->thread_index == 1)
            CHECK(WaitForSingleObject(s->sync->progress, 15000) == WAIT_OBJECT_0);
    }
}

static void on_vertex(unsigned slot, unsigned mode, void *data,
                      void *polygon_data, int replacement)
{
    tess_case *s = callback_case(slot, mode, VERTEX, polygon_data);
    CHECK(s->in_primitive);
    vertex *v = checked_vertex(s, data);
    ++s->counts[mode][VERTEX];
    ++s->primitive_vertices;
    if (replacement) ++s->replacement_vertices[mode];
    else ++s->original_vertices[mode];
    if (v->generated) ++s->generated_seen;
    if (s->sync && s->thread_index == 0) CHECK(SetEvent(s->sync->progress));
}

static void on_end(unsigned slot, unsigned mode, void *polygon_data)
{
    tess_case *s = callback_case(slot, mode, END, polygon_data);
    CHECK(s->in_primitive);
    if (s->expected_modes[VERTEX] != NO_CALLBACK) {
        CHECK(s->primitive_vertices >= 3);
        if (s->primitive_type == GL_TRIANGLES) CHECK(s->primitive_vertices % 3 == 0);
    }
    s->in_primitive = 0;
    ++s->counts[mode][END];
}

static void on_edge(unsigned slot, unsigned mode, GLboolean edge, void *polygon_data)
{
    tess_case *s = callback_case(slot, mode, EDGE, polygon_data);
    CHECK(s->in_primitive);
    CHECK(edge == GL_TRUE || edge == GL_FALSE);
    ++s->counts[mode][EDGE];
}

static void on_error(unsigned slot, unsigned mode, GLenum error, void *polygon_data)
{
    tess_case *s = callback_case(slot, mode, ERROR_CALLBACK, polygon_data);
    CHECK(s->expected_error != 0 && error == s->expected_error);
    s->expected_error = 0;
    ++s->counts[mode][ERROR_CALLBACK];
}

static void on_combine(unsigned slot, unsigned mode, GLdouble coords[3],
                       void *data[4], GLfloat weights[4], void **output,
                       void *polygon_data)
{
    tess_case *s = callback_case(slot, mode, COMBINE, polygon_data);
    CHECK(coords != NULL && data != NULL && weights != NULL && output != NULL);
    CHECK(s->generated_count < sizeof(s->generated) / sizeof(s->generated[0]));
    GLdouble total = 0, interpolated[3] = {0, 0, 0};
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(weights[i] >= 0 && weights[i] <= 1);
        total += weights[i];
        if (data[i]) {
            vertex *source = checked_vertex(s, data[i]);
            for (unsigned axis = 0; axis < 3; ++axis)
                interpolated[axis] += source->coords[axis] * weights[i];
        } else CHECK(weights[i] == 0);
    }
    CHECK(close_enough(total, 1));
    for (unsigned axis = 0; axis < 3; ++axis)
        CHECK(close_enough(coords[axis], interpolated[axis]));
    CHECK(close_enough(coords[0], 0) && close_enough(coords[1], 0));
    vertex *v = &s->generated[s->generated_count++];
    v->owner = s;
    v->serial = s->serial;
    memcpy(v->coords, coords, sizeof(v->coords));
    v->generated = 1;
    /* Storage belongs to the case and remains live through EndPolygon. */
    *output = v;
    ++s->counts[mode][COMBINE];
}

/* Windows invokes stdcall trampolines; every guest callback here is cdecl.
   In particular COMBINE has FOUR arguments, COMBINE_DATA has FIVE. */
#define DEFINE_CALLBACKS(prefix, slot) \
    static void __cdecl prefix##_begin(GLenum type) { on_begin(slot, REGULAR, type, NULL); } \
    static void __cdecl prefix##_vertex(void *v) { on_vertex(slot, REGULAR, v, NULL, 0); } \
    static void __cdecl prefix##_end(void) { on_end(slot, REGULAR, NULL); } \
    static void __cdecl prefix##_edge(GLboolean edge) { on_edge(slot, REGULAR, edge, NULL); } \
    static void __cdecl prefix##_error(GLenum error) { on_error(slot, REGULAR, error, NULL); } \
    static void __cdecl prefix##_combine(GLdouble c[3], void *v[4], GLfloat w[4], void **out) \
        { on_combine(slot, REGULAR, c, v, w, out, NULL); } \
    static void __cdecl prefix##_begin_data(GLenum type, void *p) { on_begin(slot, DATA, type, p); } \
    static void __cdecl prefix##_vertex_data(void *v, void *p) { on_vertex(slot, DATA, v, p, 0); } \
    static void __cdecl prefix##_end_data(void *p) { on_end(slot, DATA, p); } \
    static void __cdecl prefix##_edge_data(GLboolean edge, void *p) { on_edge(slot, DATA, edge, p); } \
    static void __cdecl prefix##_error_data(GLenum error, void *p) { on_error(slot, DATA, error, p); } \
    static void __cdecl prefix##_combine_data(GLdouble c[3], void *v[4], GLfloat w[4], void **out, void *p) \
        { on_combine(slot, DATA, c, v, w, out, p); } \
    static void __cdecl prefix##_replacement(void *v) { on_vertex(slot, REGULAR, v, NULL, 1); } \
    static void __cdecl prefix##_replacement_data(void *v, void *p) { on_vertex(slot, DATA, v, p, 1); }

DEFINE_CALLBACKS(first, 0)
DEFINE_CALLBACKS(second, 1)
#undef DEFINE_CALLBACKS

#define GUEST_CALLBACK(fn) ((gfx_glu_callback)(fn))
#define CALLBACK_SET(prefix) { \
    {{GUEST_CALLBACK(prefix##_begin), GUEST_CALLBACK(prefix##_vertex), GUEST_CALLBACK(prefix##_end), \
      GUEST_CALLBACK(prefix##_edge), GUEST_CALLBACK(prefix##_error), GUEST_CALLBACK(prefix##_combine)}, \
     {GUEST_CALLBACK(prefix##_begin_data), GUEST_CALLBACK(prefix##_vertex_data), GUEST_CALLBACK(prefix##_end_data), \
      GUEST_CALLBACK(prefix##_edge_data), GUEST_CALLBACK(prefix##_error_data), GUEST_CALLBACK(prefix##_combine_data)}}, \
    {GUEST_CALLBACK(prefix##_replacement), GUEST_CALLBACK(prefix##_replacement_data)} \
}

static const struct {
    gfx_glu_callback functions[2][CALLBACK_COUNT];
    gfx_glu_callback replacement[2];
} callback_sets[2] = {CALLBACK_SET(first), CALLBACK_SET(second)};
#undef CALLBACK_SET

static void register_callbacks(tess_case *s, unsigned mode)
{
    for (unsigned kind = 0; kind < CALLBACK_COUNT; ++kind)
        api.gluTessCallback(s->tess, selectors[mode][kind],
                            callback_sets[s->slot].functions[mode][kind]);
}

static void unregister_callbacks(tess_case *s)
{
    for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned kind = 0; kind < CALLBACK_COUNT; ++kind)
            api.gluTessCallback(s->tess, selectors[mode][kind], NULL);
}

static void create_case(tess_case *s, unsigned slot)
{
    s->slot = slot;
    s->owner_thread = GetCurrentThreadId();
    s->tess = api.gluNewTess();
    CHECK(s->tess != NULL);
    api.gluTessNormal(s->tess, 0, 0, 1);
    api.gluTessProperty(s->tess, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_ODD);
    GLdouble winding = 0;
    api.gluGetTessProperty(s->tess, GLU_TESS_WINDING_RULE, &winding);
    CHECK(winding == GLU_TESS_WINDING_ODD);
}

static void tessellate(tess_case *s, unsigned mode, unsigned vertex_mode,
                      enum polygon_kind polygon)
{
    static const GLdouble shapes[2][4][2] = {
        {{-1, -1}, {1, 1}, {-1, 1}, {1, -1}},
        {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}
    };
    const GLdouble (*coordinates)[2] = shapes[polygon == SQUARE];
    int missing_combine = polygon == WITHOUT_COMBINE;
    unsigned before[2][CALLBACK_COUNT];
    memcpy(before, s->counts, sizeof(before));
    unsigned generated_before = s->generated_seen;
    tess_case *previous = active_case;
    active_case = s;
    CHECK(!s->in_primitive);
    ++s->serial;
    s->polygon.owner = s;
    s->polygon.serial = s->serial;
    s->generated_count = 0;
    s->expected_error = 0;
    for (unsigned kind = 0; kind < CALLBACK_COUNT; ++kind) s->expected_modes[kind] = mode;
    s->expected_modes[VERTEX] = vertex_mode;
    if (missing_combine) s->expected_modes[COMBINE] = NO_CALLBACK;
    for (unsigned i = 0; i < 4; ++i) {
        vertex *v = &s->input[i];
        v->owner = s;
        v->serial = s->serial;
        v->coords[0] = coordinates[i][0];
        v->coords[1] = coordinates[i][1];
        v->coords[2] = 0;
        v->generated = 0;
    }
    api.gluTessBeginPolygon(s->tess, &s->polygon);
    /* ERROR_DATA must receive this polygon's data, including on a property
       error delivered before the contour has started. */
    if (mode != NO_CALLBACK) s->expected_error = GLU_INVALID_ENUM;
    api.gluTessProperty(s->tess, (GLenum)0xffffffffu, 0);
    CHECK(s->expected_error == 0);
    api.gluTessBeginContour(s->tess);
    for (unsigned i = 0; i < 4; ++i)
        api.gluTessVertex(s->tess, s->input[i].coords, &s->input[i]);
    api.gluTessEndContour(s->tess);
    if (missing_combine) s->expected_error = GLU_TESS_NEED_COMBINE_CALLBACK;
    uintptr_t stack_before, stack_after;
    __asm__ volatile("movl %%esp, %0" : "=r"(stack_before) : : "memory");
    api.gluTessEndPolygon(s->tess);
    __asm__ volatile("movl %%esp, %0" : "=r"(stack_after) : : "memory");
    CHECK(stack_before == stack_after);
    ++s->stack_checks;
    CHECK(s->expected_error == 0 && !s->in_primitive);
    for (unsigned callback_mode = 0; callback_mode < 2; ++callback_mode)
        for (unsigned kind = 0; kind < CALLBACK_COUNT; ++kind)
            if (s->expected_modes[kind] != callback_mode)
                CHECK(s->counts[callback_mode][kind] == before[callback_mode][kind]);
    if (mode != NO_CALLBACK) {
        CHECK(s->counts[mode][ERROR_CALLBACK] - before[mode][ERROR_CALLBACK] == (missing_combine ? 2u : 1u));
        if (missing_combine) {
            CHECK(s->counts[mode][BEGIN] == before[mode][BEGIN]);
            CHECK(s->generated_count == 0);
        } else {
            unsigned begins = s->counts[mode][BEGIN] - before[mode][BEGIN];
            CHECK(begins > 0 && s->counts[mode][END] - before[mode][END] == begins);
            CHECK(s->counts[mode][EDGE] > before[mode][EDGE]);
            CHECK(s->counts[mode][COMBINE] > before[mode][COMBINE]);
            CHECK(s->generated_count > 0);
            if (vertex_mode != NO_CALLBACK) {
                CHECK(s->counts[vertex_mode][VERTEX] - before[vertex_mode][VERTEX] >= 6);
                CHECK(s->generated_seen > generated_before);
            }
        }
    } else CHECK(s->generated_count == 0);
    ++s->polygons;
    active_case = previous;
}

typedef struct {
    tess_case state;
    unsigned mode;
    unsigned iterations;
} thread_job;

static DWORD WINAPI tess_thread(void *argument)
{
    thread_job *job = argument;
    tess_case *s = &job->state;
    create_case(s, s->slot);
    register_callbacks(s, job->mode);
    for (unsigned i = 0; i < job->iterations; ++i)
        tessellate(s, job->mode, job->mode, BOWTIE);
    api.gluDeleteTess(s->tess);
    s->tess = NULL;
    CHECK(active_case == NULL);
    return 0;
}

typedef struct {
    unsigned kind;
    DWORD owner_thread;
    unsigned original;
    unsigned replacement;
    GLenum last_error;
    int enabled;
} object_error_case;

static __declspec(thread) object_error_case *active_error;

static void object_error(unsigned kind, int replacement, GLenum error)
{
    object_error_case *s = active_error;
    CHECK(s != NULL && s->enabled && s->kind == kind);
    CHECK(s->owner_thread == GetCurrentThreadId());
    if (kind == 0) CHECK(error == GLU_INVALID_ENUM);
    else CHECK(error == GLU_INVALID_ENUM || error == GLU_INVALID_VALUE ||
               (error >= GLU_NURBS_ERROR1 && error <= GLU_NURBS_ERROR37));
    CHECK(api.gluErrorString(error) != NULL);
    s->last_error = error;
    if (replacement) ++s->replacement;
    else ++s->original;
}

static void __cdecl quadric_error(GLenum error) { object_error(0, 0, error); }
static void __cdecl quadric_replacement(GLenum error) { object_error(0, 1, error); }
static void __cdecl nurbs_error(GLenum error) { object_error(1, 0, error); }
static void __cdecl nurbs_replacement(GLenum error) { object_error(1, 1, error); }

static void test_object_errors(object_error_case *q, object_error_case *n)
{
    q->kind = 0;
    q->owner_thread = GetCurrentThreadId();
    q->enabled = 1;
    active_error = q;
    GLUquadric *quadric = api.gluNewQuadric();
    CHECK(quadric != NULL);
    api.gluQuadricCallback(quadric, GLU_ERROR, GUEST_CALLBACK(quadric_error));
    api.gluQuadricNormals(quadric, (GLenum)0xffffffffu);
    CHECK(q->original == 1 && q->replacement == 0);
    api.gluQuadricCallback(quadric, GLU_ERROR, GUEST_CALLBACK(quadric_replacement));
    api.gluQuadricOrientation(quadric, (GLenum)0xffffffffu);
    CHECK(q->original == 1 && q->replacement == 1);
    api.gluQuadricCallback(quadric, GLU_ERROR, NULL);
    q->enabled = 0;
    api.gluQuadricNormals(quadric, (GLenum)0xffffffffu);
    CHECK(q->original == 1 && q->replacement == 1);
    q->enabled = 1;
    api.gluQuadricCallback(quadric, GLU_ERROR, GUEST_CALLBACK(quadric_error));
    api.gluQuadricNormals(quadric, (GLenum)0xffffffffu);
    CHECK(q->original == 2 && q->replacement == 1);
    api.gluDeleteQuadric(quadric);

    n->kind = 1;
    n->owner_thread = GetCurrentThreadId();
    n->enabled = 1;
    active_error = n;
    GLUnurbs *nurbs = api.gluNewNurbsRenderer();
    CHECK(nurbs != NULL);
    api.gluNurbsCallback(nurbs, GLU_ERROR, GUEST_CALLBACK(nurbs_error));
    api.gluNurbsProperty(nurbs, (GLenum)0xffffffffu, 0);
    CHECK(n->original == 1 && n->replacement == 0);
    api.gluNurbsCallback(nurbs, GLU_ERROR, GUEST_CALLBACK(nurbs_replacement));
    api.gluNurbsProperty(nurbs, GLU_DISPLAY_MODE, -1);
    CHECK(n->original == 1 && n->replacement == 1);
    api.gluNurbsCallback(nurbs, GLU_ERROR, NULL);
    n->enabled = 0;
    api.gluNurbsProperty(nurbs, (GLenum)0xffffffffu, 0);
    CHECK(n->original == 1 && n->replacement == 1);
    n->enabled = 1;
    api.gluNurbsCallback(nurbs, GLU_ERROR, GUEST_CALLBACK(nurbs_error));
    api.gluNurbsProperty(nurbs, (GLenum)0xffffffffu, 0);
    CHECK(n->original == 2 && n->replacement == 1);
    api.gluDeleteNurbsRenderer(nurbs);
    active_error = NULL;
}

int main(void)
{
    char dll_path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, dll_path, sizeof(dll_path));
    CHECK(length != 0 && length < sizeof(dll_path));
    char *slash = strrchr(dll_path, '\\');
    CHECK(slash != NULL);
    CHECK((size_t)(slash - dll_path) + sizeof("\\win\\lxgfx.dll") <= sizeof(dll_path));
    strcpy(slash, "\\win\\lxgfx.dll");
    HMODULE dll = LoadLibraryA(dll_path);
    CHECK(dll != NULL);
    lx_provider_symbols_fn provider = (lx_provider_symbols_fn)GetProcAddress(dll, "lx_provider_symbols");
    CHECK(provider != NULL);
    symbols = provider(&symbol_count);
    CHECK(symbols != NULL && symbol_count != 0);
#define LOAD_API(name) api.name = (__typeof__(&lx_##name))find_symbol(#name);
    GLU_APIS(LOAD_API)
#undef LOAD_API

    tess_case first_case = {0}, second_case = {0};
    create_case(&first_case, 0);
    create_case(&second_case, 1);
    CHECK(first_case.tess != second_case.tess);
    register_callbacks(&first_case, REGULAR);
    register_callbacks(&second_case, REGULAR);
    register_callbacks(&second_case, DATA);
    for (unsigned i = 0; i < 64; ++i) {
        tessellate(&first_case, REGULAR, REGULAR, BOWTIE);
        /* DATA must supersede the registered regular callbacks. */
        tessellate(&second_case, DATA, DATA, BOWTIE);
    }

    unsigned original = first_case.original_vertices[REGULAR];
    api.gluTessCallback(first_case.tess, GLU_TESS_VERTEX, callback_sets[0].replacement[REGULAR]);
    tessellate(&first_case, REGULAR, REGULAR, BOWTIE);
    CHECK(first_case.original_vertices[REGULAR] == original);
    CHECK(first_case.replacement_vertices[REGULAR] > 0);
    original = second_case.original_vertices[DATA];
    api.gluTessCallback(second_case.tess, GLU_TESS_VERTEX_DATA, callback_sets[1].replacement[DATA]);
    tessellate(&second_case, DATA, DATA, BOWTIE);
    CHECK(second_case.original_vertices[DATA] == original);
    CHECK(second_case.replacement_vertices[DATA] > 0);

    api.gluTessCallback(first_case.tess, GLU_TESS_VERTEX, NULL);
    tessellate(&first_case, REGULAR, NO_CALLBACK, BOWTIE);
    /* Unregistering DATA_VERTEX must fall back to regular VERTEX, not leave
       a stale DATA trampoline or suppress the regular callback. */
    api.gluTessCallback(second_case.tess, GLU_TESS_VERTEX_DATA, NULL);
    unsigned replacement = second_case.replacement_vertices[DATA];
    tessellate(&second_case, DATA, REGULAR, BOWTIE);
    CHECK(second_case.replacement_vertices[DATA] == replacement);
    CHECK(second_case.original_vertices[REGULAR] > 0);
    register_callbacks(&first_case, REGULAR);
    api.gluTessCallback(first_case.tess, GLU_TESS_COMBINE, NULL);
    tessellate(&first_case, REGULAR, REGULAR, WITHOUT_COMBINE);
    register_callbacks(&second_case, DATA);
    tessellate(&second_case, DATA, DATA, BOWTIE);

    unregister_callbacks(&first_case);
    unregister_callbacks(&second_case);
    /* The bowtie detects a stale COMBINE callback; the square still emits
       primitives without COMBINE, exposing stale BEGIN/VERTEX/END/EDGE. */
    tessellate(&first_case, NO_CALLBACK, NO_CALLBACK, BOWTIE);
    tessellate(&second_case, NO_CALLBACK, NO_CALLBACK, BOWTIE);
    tessellate(&first_case, NO_CALLBACK, NO_CALLBACK, SQUARE);
    tessellate(&second_case, NO_CALLBACK, NO_CALLBACK, SQUARE);
    register_callbacks(&first_case, REGULAR);
    register_callbacks(&second_case, REGULAR);
    register_callbacks(&second_case, DATA);
    first_case.nested = &second_case;
    tessellate(&first_case, REGULAR, REGULAR, BOWTIE);
    CHECK(first_case.nested_runs == 1);
    first_case.nested = NULL;
    CHECK(active_case == NULL);

    thread_sync sync = {0};
    sync.entered[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
    sync.entered[1] = CreateEventA(NULL, TRUE, FALSE, NULL);
    sync.progress = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(sync.entered[0] != NULL && sync.entered[1] != NULL && sync.progress != NULL);
    thread_job jobs[2] = {0};
    HANDLE threads[2];
    for (unsigned i = 0; i < 2; ++i) {
        jobs[i].state.slot = i;
        jobs[i].state.sync = &sync;
        jobs[i].state.thread_index = i;
        jobs[i].mode = i;
        jobs[i].iterations = 128;
        threads[i] = CreateThread(NULL, 0, tess_thread, &jobs[i], 0, NULL);
        CHECK(threads[i] != NULL);
    }
    CHECK(WaitForMultipleObjects(2, threads, TRUE, 60000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 2; ++i) {
        DWORD result = 1;
        CHECK(GetExitCodeThread(threads[i], &result) && result == 0);
        CHECK(jobs[i].state.rendezvous_done);
        CHECK(jobs[i].state.polygons == jobs[i].iterations);
        CHECK(CloseHandle(threads[i]));
        CHECK(CloseHandle(sync.entered[i]));
    }
    CHECK(CloseHandle(sync.progress));

    object_error_case quadric = {0}, nurbs = {0};
    test_object_errors(&quadric, &nurbs);
    api.gluDeleteTess(first_case.tess);
    api.gluDeleteTess(second_case.tess);

    const tess_case *cases[] = {&first_case, &second_case, &jobs[0].state, &jobs[1].state};
    unsigned totals[2][CALLBACK_COUNT] = {{0}};
    unsigned polygons = 0, stack_checks = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        polygons += cases[i]->polygons;
        stack_checks += cases[i]->stack_checks;
        for (unsigned mode = 0; mode < 2; ++mode)
            for (unsigned kind = 0; kind < CALLBACK_COUNT; ++kind)
                totals[mode][kind] += cases[i]->counts[mode][kind];
    }
    CHECK(polygons == stack_checks);
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (unsigned kind = 0; kind < CALLBACK_COUNT; ++kind) CHECK(totals[mode][kind] > 0);
        printf("PASS tess %s begin=%u vertex=%u end=%u edge=%u error=%u combine=%u\n",
               mode == REGULAR ? "regular" : "DATA", totals[mode][BEGIN], totals[mode][VERTEX],
               totals[mode][END], totals[mode][EDGE], totals[mode][ERROR_CALLBACK], totals[mode][COMBINE]);
    }
    printf("PASS polygons=%u ESP_checks=%u nested=%u threaded=%u substituted_vertices=%u/%u "
           "quadric_errors=%u nurbs_errors=%u(last=%u) unregister=PASS\n",
           polygons, stack_checks, first_case.nested_runs,
           jobs[0].state.polygons + jobs[1].state.polygons,
           first_case.replacement_vertices[REGULAR], second_case.replacement_vertices[DATA],
           quadric.original + quadric.replacement, nurbs.original + nurbs.replacement,
           nurbs.last_error);
    CHECK(FreeLibrary(dll));
    return 0;
}
