#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
void ufbxt_assert_fail(const char *file, uint32_t line, const char *expr);

#define ufbx_assert(cond) do { \
		if (!(cond)) ufbxt_assert_fail(__FILE__, __LINE__, "Internal assert: " #cond); \
	} while (0)

#define ufbxt_arraycount(arr) (sizeof(arr) / sizeof(*(arr)))

#undef ufbx_assert

#include "../ufbx.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdarg.h>
#include <math.h>

#if defined(_OPENMP)
	#include <omp.h>
#else
	static int omp_get_thread_num() { return 0; }
	static int omp_get_num_threads() { return 1; }
#endif

// -- Thread local

#ifdef _MSC_VER
	#define ufbxt_threadlocal __declspec(thread)
#else
	#define ufbxt_threadlocal __thread
#endif

// -- Timing

typedef struct {
	uint64_t os_tick;
	uint64_t cpu_tick;
} cputime_sync_point;

typedef struct {
	cputime_sync_point begin, end;
	uint64_t os_freq;
	uint64_t cpu_freq;
	double rcp_os_freq;
	double rcp_cpu_freq;
} cputime_sync_span;

extern const cputime_sync_span *cputime_default_sync;

void cputime_begin_init();
void cputime_end_init();
void cputime_init();

void cputime_begin_sync(cputime_sync_span *span);
void cputime_end_sync(cputime_sync_span *span);

uint64_t cputime_cpu_tick();
uint64_t cputime_os_tick();

double cputime_cpu_delta_to_sec(const cputime_sync_span *span, uint64_t cpu_delta);
double cputime_os_delta_to_sec(const cputime_sync_span *span, uint64_t os_delta);
double cputime_cpu_tick_to_sec(const cputime_sync_span *span, uint64_t cpu_tick);
double cputime_os_tick_to_sec(const cputime_sync_span *span, uint64_t os_tick);

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

void cputime_sync_now(cputime_sync_point *sync, int accuracy)
{
	uint64_t best_delta = UINT64_MAX;
	uint64_t os_tick = 0, cpu_tick = 0;

	int runs = accuracy ? accuracy : 100;
	for (int i = 0; i < runs; i++) {
		LARGE_INTEGER begin, end;
		QueryPerformanceCounter(&begin);
		uint64_t cycle = __rdtsc();
		QueryPerformanceCounter(&end);

		uint64_t delta = end.QuadPart - begin.QuadPart;
		if (delta < best_delta) {
			os_tick = (begin.QuadPart + end.QuadPart) / 2;
			cpu_tick = cycle;
		}

		if (delta == 0) break;
	}

	sync->cpu_tick = cpu_tick;
	sync->os_tick = os_tick;
}

uint64_t cputime_cpu_tick()
{
	return __rdtsc();
}

uint64_t cputime_os_tick()
{
	LARGE_INTEGER res;
	QueryPerformanceCounter(&res);
	return res.QuadPart;
}

static uint64_t cputime_os_freq()
{
	LARGE_INTEGER res;
	QueryPerformanceFrequency(&res);
	return res.QuadPart;
}

static void cputime_os_wait()
{
	Sleep(1);
}

#else

#include <time.h>
// TODO: Other architectures
#include <x86intrin.h>

void cputime_sync_now(cputime_sync_point *sync, int accuracy)
{
	uint64_t best_delta = UINT64_MAX;
	uint64_t os_tick, cpu_tick;

	struct timespec begin, end;

	int runs = accuracy ? accuracy : 100;
	for (int i = 0; i < runs; i++) {
		clock_gettime(CLOCK_REALTIME, &begin);
		uint64_t cycle = (uint64_t)__rdtsc();
		clock_gettime(CLOCK_REALTIME, &end);

		uint64_t begin_ns = (uint64_t)begin.tv_sec*UINT64_C(1000000000) + (uint64_t)begin.tv_nsec;
		uint64_t end_ns = (uint64_t)end.tv_sec*UINT64_C(1000000000) + (uint64_t)end.tv_nsec;

		uint64_t delta = end_ns - begin_ns;
		if (delta < best_delta) {
			os_tick = (begin_ns + end_ns) / 2;
			cpu_tick = cycle;
		}

		if (delta == 0) break;
	}

	sync->cpu_tick = cpu_tick;
	sync->os_tick = os_tick;
}

uint64_t cputime_cpu_tick()
{
	return (uint64_t)__rdtsc();
}

uint64_t cputime_os_tick()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec*UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t cputime_os_freq()
{
	return UINT64_C(1000000000);
}

static void cputime_os_wait()
{
	struct timespec duration;
	duration.tv_sec = 0;
	duration.tv_nsec = 1000000000l;
	nanosleep(&duration, NULL);
}

#endif

static cputime_sync_span g_cputime_sync;
const cputime_sync_span *cputime_default_sync = &g_cputime_sync;

void cputime_begin_init()
{
	cputime_begin_sync(&g_cputime_sync);
}

void cputime_end_init()
{
	cputime_end_sync(&g_cputime_sync);
}

void cputime_init()
{
	cputime_begin_init();
	cputime_end_init();
}

void cputime_begin_sync(cputime_sync_span *span)
{
	cputime_sync_now(&span->begin, 0);
}

void cputime_end_sync(cputime_sync_span *span)
{
	uint64_t os_freq = cputime_os_freq();

	uint64_t min_span = os_freq / 1000;
	uint64_t os_tick = cputime_os_tick();
	while (os_tick - span->begin.os_tick <= min_span) {
		cputime_os_wait();
		os_tick = cputime_os_tick();
	}

	cputime_sync_now(&span->end, 0);
	uint64_t len_os = span->end.os_tick - span->begin.os_tick;
	uint64_t len_cpu = span->end.cpu_tick - span->begin.cpu_tick;
	double cpu_freq = (double)len_cpu / (double)len_os * (double)os_freq;

	span->os_freq = os_freq;
	span->cpu_freq = (uint64_t)cpu_freq;
	span->rcp_os_freq = 1.0 / (double)os_freq;
	span->rcp_cpu_freq = 1.0 / cpu_freq;
}

double cputime_cpu_delta_to_sec(const cputime_sync_span *span, uint64_t cpu_delta)
{
	if (!span) span = &g_cputime_sync;
	return (double)cpu_delta * span->rcp_cpu_freq;
}

double cputime_os_delta_to_sec(const cputime_sync_span *span, uint64_t os_delta)
{
	if (!span) span = &g_cputime_sync;
	return (double)os_delta * span->rcp_os_freq;
}

double cputime_cpu_tick_to_sec(const cputime_sync_span *span, uint64_t cpu_tick)
{
	if (!span) span = &g_cputime_sync;
	return (double)(cpu_tick - span->begin.cpu_tick) * span->rcp_cpu_freq;
}

double cputime_os_tick_to_sec(const cputime_sync_span *span, uint64_t os_tick)
{
	if (!span) span = &g_cputime_sync;
	return (double)(os_tick - span->begin.os_tick) * span->rcp_os_freq;
}

// -- Test framework

#define ufbxt_memory_context(data) \
	ufbxt_make_memory_context(data, (uint32_t)sizeof(data) - 1)
#define ufbxt_memory_context_values(data) \
	ufbxt_make_memory_context_values(data, (uint32_t)sizeof(data) - 1)

#define ufbxt_assert(cond) do { \
		if (!(cond)) ufbxt_assert_fail(__FILE__, __LINE__, #cond); \
	} while (0)

#define ufbxt_assert_eq(a, b, size) do { \
		ufbxt_assert_eq_test(a, b, size, __FILE__, __LINE__, \
			"ufbxt_assert_eq(" #a ", " #b ", " #size ")"); \
	} while (0)

typedef struct {
	int failed;
	const char *file;
	uint32_t line;
	const char *expr;
} ufbxt_fail;

typedef struct {
	const char *name;
	void (*func)(void);

	ufbxt_fail fail;
} ufbxt_test;

ufbxt_test *g_current_test;
uint64_t g_bechmark_begin_tick;

ufbx_error g_error;
jmp_buf g_test_jmp;
int g_verbose;

char g_log_buf[16*1024];
uint32_t g_log_pos;

char g_hint[8*1024];

bool g_skip_print_ok = false;

typedef struct {
	const char *test_name;
	uint16_t test_version;
	uint8_t patch_value;
	uint32_t patch_offset;
	uint32_t temp_limit;
	uint32_t result_limit;
	uint32_t truncate_length;
	const char *description;
} ufbxt_check_line;

static ufbxt_check_line g_checks[16384];

ufbxt_threadlocal jmp_buf *t_jmp_buf;

void ufbxt_assert_fail(const char *file, uint32_t line, const char *expr)
{
	if (t_jmp_buf) {
		longjmp(g_test_jmp, 1);
	}

	printf("FAIL\n");
	fflush(stdout);

	g_current_test->fail.failed = 1;
	g_current_test->fail.file = file;
	g_current_test->fail.line = line;
	g_current_test->fail.expr = expr;

	longjmp(g_test_jmp, 1);
}

void ufbxt_logf(const char *fmt, ...)
{
	if (!g_verbose) return;

	va_list args;
	va_start(args, fmt);
	if (g_log_pos < sizeof(g_log_buf)) {
		g_log_pos += vsnprintf(g_log_buf + g_log_pos,
			sizeof(g_log_buf) - g_log_pos, fmt, args);
		if (g_log_pos < sizeof(g_log_buf)) {
			g_log_buf[g_log_pos] = '\n';
			g_log_pos++;
		}
	}
	va_end(args);
}

void ufbxt_hintf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(g_hint, sizeof(g_hint), fmt, args);
	va_end(args);
}

void ufbxt_assert_eq_test(const void *a, const void *b, size_t size, const char *file, uint32_t line, const char *expr)
{
	const char *ac = (const char *)a;
	const char *bc = (const char *)b;
	for (size_t i = 0; i < size; i++) {
		if (ac[i] == bc[i]) continue;

		ufbxt_logf("Byte offset %u: 0x%02x != 0x%02x\n", (uint32_t)i, (uint8_t)ac[i], (uint8_t)bc[i]);
		ufbxt_assert_fail(file, line, expr);
	}
}

void ufbxt_log_flush()
{
	int prev_newline = 1;
	for (uint32_t i = 0; i < g_log_pos; i++) {
		if (i >= sizeof(g_log_buf)) break;
		char ch = g_log_buf[i];
		if (ch == '\n') {
			putchar('\n');
			prev_newline = 1;
		} else {
			if (prev_newline) {
				putchar(' ');
				putchar(' ');
			}
			prev_newline = 0;
			putchar(ch);
		}
	}
	g_log_pos = 0;
}

void ufbxt_log_error(ufbx_error *err)
{
	if (!err) return;
	for (size_t i = 0; i < err->stack_size; i++) {
		ufbx_error_frame *f = &err->stack[i];
		ufbxt_logf("Line %u %s: %s", f->source_line, f->function, f->description);
	}
}

void ufbxt_bechmark_begin()
{
	g_bechmark_begin_tick = cputime_cpu_tick();
}

double ufbxt_bechmark_end()
{
	uint64_t end_tick = cputime_cpu_tick();
	uint64_t delta = end_tick - g_bechmark_begin_tick;
	double sec = cputime_cpu_delta_to_sec(NULL, delta);
	double ghz = (double)cputime_default_sync->cpu_freq / 1e9;
	ufbxt_logf("%.3fms / %ukcy at %.2fGHz", sec * 1e3, (uint32_t)(delta / 1000), ghz);
	return sec;
}

char data_root[256];

static void *ufbxt_read_file(const char *name, size_t *p_size)
{
	FILE *file = fopen(name, "rb");
	if (!file) return NULL;

	fseek(file, 0, SEEK_END);
	size_t size = ftell(file);
	fseek(file, 0, SEEK_SET);

	char *data = malloc(size + 1);
	ufbxt_assert(data != NULL);
	size_t num_read = fread(data, 1, size, file);
	fclose(file);

	data[size] = '\0';

	if (num_read != size) {
		ufbxt_assert_fail(__FILE__, __LINE__, "Failed to load file");
	}

	*p_size = size;
	return data;
}

typedef struct {
	char name[64];

	size_t num_faces;
	size_t num_indices;

	ufbx_face *faces;

	ufbx_vertex_vec3 vertex_position;
	ufbx_vertex_vec3 vertex_normal;
	ufbx_vertex_vec2 vertex_uv;
} ufbxt_obj_mesh;

typedef struct {

	ufbxt_obj_mesh *meshes;
	size_t num_meshes;

} ufbxt_obj_file;

static ufbxt_obj_file *ufbxt_load_obj(void *obj_data, size_t obj_size)
{
	size_t num_positions = 0;
	size_t num_normals = 0;
	size_t num_uvs = 0;
	size_t num_faces = 0;
	size_t num_meshes = 0;

	char *line = (char*)obj_data;
	for (;;) {
		char *end = strpbrk(line, "\r\n");
		char prev = '\0';
		if (end) {
			prev = *end;
			*end = '\0';
		}

		if (!strncmp(line, "v ", 2)) num_positions++;
		else if (!strncmp(line, "vt ", 3)) num_uvs++;
		else if (!strncmp(line, "vn ", 3)) num_normals++;
		else if (!strncmp(line, "f ", 2)) num_faces++;
		else if (!strncmp(line, "g default", 7)) { /* ignore default group */ }
		else if (!strncmp(line, "g ", 2)) num_meshes++;

		if (end) {
			*end = prev;
			line = end + 1;
		} else {
			break;
		}
	}

	size_t alloc_size = 0;
	alloc_size += sizeof(ufbxt_obj_file);
	alloc_size += num_positions * sizeof(ufbx_vec3);
	alloc_size += num_normals * sizeof(ufbx_vec3);
	alloc_size += num_uvs * sizeof(ufbx_vec2);
	alloc_size += num_faces * sizeof(ufbx_face);
	alloc_size += num_faces * 3 * 4 * sizeof(int32_t);
	alloc_size += num_meshes * sizeof(ufbxt_obj_mesh);

	void *data = malloc(alloc_size);
	ufbxt_assert(data);

	ufbxt_obj_file *obj = (ufbxt_obj_file*)data;
	ufbx_vec3 *positions = (ufbx_vec3*)(obj + 1);
	ufbx_vec3 *normals = (ufbx_vec3*)(positions + num_positions);
	ufbx_vec2 *uvs = (ufbx_vec2*)(normals + num_normals);
	ufbx_face *faces = (ufbx_face*)(uvs + num_uvs);
	int32_t *position_indices = (int32_t*)(faces + num_faces);
	int32_t *normal_indices = (int32_t*)(position_indices + num_faces * 4);
	int32_t *uv_indices = (int32_t*)(normal_indices + num_faces * 4);
	ufbxt_obj_mesh *meshes = (ufbxt_obj_mesh*)(uv_indices + num_faces * 4);
	void *data_end = meshes + num_meshes;
	ufbxt_assert((char*)data_end - (char*)data == alloc_size);

	ufbx_vec3 *dp = positions;
	ufbx_vec3 *dn = normals;
	ufbx_vec2 *du = uvs;
	ufbxt_obj_mesh *mesh = NULL;

	int32_t *dpi = position_indices;
	int32_t *dni = normal_indices;
	int32_t *dui = uv_indices;

	ufbx_face *df = faces;

	obj->meshes = meshes;
	obj->num_meshes = num_meshes;

	line = (char*)obj_data;
	for (;;) {
		char *line_end = strpbrk(line, "\r\n");
		char prev = '\0';
		if (line_end) {
			prev = *line_end;
			*line_end = '\0';
		}

		if (!strncmp(line, "v ", 2)) {
			ufbxt_assert(sscanf(line, "v %lf %lf %lf", &dp->x, &dp->y, &dp->z) == 3);
			dp++;
		} else if (!strncmp(line, "vt ", 3)) {
			ufbxt_assert(sscanf(line, "vt %lf %lf", &du->x, &du->y) == 2);
			du++;
		} else if (!strncmp(line, "vn ", 3)) {
			ufbxt_assert(sscanf(line, "vn %lf %lf %lf", &dn->x, &dn->y, &dn->z) == 3);
			dn++;
		} else if (!strncmp(line, "f ", 2)) {
			ufbxt_assert(mesh);

			df->index_begin = (uint32_t)mesh->num_indices;
			df->num_indices = 0;

			char *begin = line + 2;
			do {
				char *end = strchr(begin, ' ');
				if (end) *end++ = '\0';

				int pi = 0, ui = 0, ni = 0;
				if (sscanf(begin, "%d/%d/%d", &pi, &ui, &ni) == 3) {
				} else if (sscanf(begin, "%d//%d", &pi, &ni) == 2) {
				} else {
					ufbxt_assert(0 && "Failed to parse face indices");
				}

				*dpi++ = pi - 1;
				*dni++ = ni - 1;
				*dui++ = ui - 1;
				mesh->num_indices++;
				df->num_indices++;

				begin = end;
			} while (begin);

			mesh->num_faces++;
			df++;
		} else if (!strncmp(line, "g default", 7)) {
			/* ignore default group */
		} else if (!strncmp(line, "g ", 2)) {
			mesh = mesh ? mesh + 1 : meshes;
			memset(mesh, 0, sizeof(ufbxt_obj_mesh));

			// HACK: Truncate name at '_' to separate Blender
			// model and mesh names
			size_t len = strcspn(line + 2, "_");

			ufbxt_assert(len < sizeof(mesh->name));
			memcpy(mesh->name, line + 2, len);
			mesh->faces = df;
			mesh->vertex_position.data = positions;
			mesh->vertex_normal.data = normals;
			mesh->vertex_uv.data = uvs;
			mesh->vertex_position.indices = dpi;
			mesh->vertex_normal.indices = dni;
			mesh->vertex_uv.indices = dui;
		}

		if (line_end) {
			*line_end = prev;
			line = line_end + 1;
		} else {
			break;
		}
	}

	return obj;
}

typedef struct {
	size_t num;
	ufbx_real sum;
	ufbx_real max;
} ufbxt_diff_error;

static void ufbxt_assert_close_real(ufbxt_diff_error *p_err, ufbx_real a, ufbx_real b)
{
	ufbx_real err = fabs(a - b);
	ufbxt_assert(err < 0.001);
	p_err->num++;
	p_err->sum += err;
	if (err > p_err->max) p_err->max = err;
}

static void ufbxt_assert_close_vec2(ufbxt_diff_error *p_err, ufbx_vec2 a, ufbx_vec2 b)
{
	ufbxt_assert_close_real(p_err, a.x, b.x);
	ufbxt_assert_close_real(p_err, a.y, b.y);
}

static void ufbxt_assert_close_vec3(ufbxt_diff_error *p_err, ufbx_vec3 a, ufbx_vec3 b)
{
	ufbxt_assert_close_real(p_err, a.x, b.x);
	ufbxt_assert_close_real(p_err, a.y, b.y);
	ufbxt_assert_close_real(p_err, a.z, b.z);
}

static void ufbxt_assert_close_vec4(ufbxt_diff_error *p_err, ufbx_vec4 a, ufbx_vec4 b)
{
	ufbxt_assert_close_real(p_err, a.x, b.x);
	ufbxt_assert_close_real(p_err, a.y, b.y);
	ufbxt_assert_close_real(p_err, a.z, b.z);
	ufbxt_assert_close_real(p_err, a.w, b.w);
}

static void ufbxt_diff_to_obj(ufbx_scene *scene, ufbxt_obj_file *obj, ufbxt_diff_error *p_err)
{
	for (size_t mesh_i = 0; mesh_i < obj->num_meshes; mesh_i++) {
		ufbxt_obj_mesh *obj_mesh = &obj->meshes[mesh_i];
		ufbx_mesh *mesh = ufbx_find_mesh(scene, obj_mesh->name);
		ufbxt_assert(mesh);

		ufbxt_assert(obj_mesh->num_faces == mesh->num_faces);
		ufbxt_assert(obj_mesh->num_indices == mesh->num_indices);

		ufbx_matrix *mat = &mesh->node.to_root;

		// Assume that the indices are in the same order!
		for (size_t face_ix = 0; face_ix < mesh->num_faces; face_ix++) {
			ufbx_face obj_face = obj_mesh->faces[face_ix];
			ufbx_face face = mesh->faces[face_ix];
			ufbxt_assert(obj_face.index_begin == face.index_begin);
			ufbxt_assert(obj_face.num_indices == face.num_indices);

			for (size_t ix = 0; ix < face.num_indices; ix++) {
				ufbx_vec3 op = ufbx_get_vertex_vec3(&obj_mesh->vertex_position, ix);
				ufbx_vec3 fp = ufbx_get_vertex_vec3(&mesh->vertex_position, ix);
				ufbx_vec3 on = ufbx_get_vertex_vec3(&obj_mesh->vertex_normal, ix);
				ufbx_vec3 fn = ufbx_get_vertex_vec3(&mesh->vertex_normal, ix);

				fp = ufbx_transform_position(mat, fp);
				fn = ufbx_transform_normal(mat, fn);

				ufbx_real fn_len = sqrt(fn.x*fn.x + fn.y*fn.y + fn.z*fn.z);
				fn.x /= fn_len;
				fn.y /= fn_len;
				fn.z /= fn_len;

				ufbxt_assert_close_vec3(p_err, op, fp);
				ufbxt_assert_close_vec3(p_err, on, fn);

				if (mesh->vertex_uv.data) {
					ufbxt_assert(obj_mesh->vertex_uv.data);
					ufbx_vec2 ou = ufbx_get_vertex_vec2(&obj_mesh->vertex_uv, ix);
					ufbx_vec2 fu = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
					ufbxt_assert_close_vec2(p_err, ou, fu);
				}
			}
		}
	}
}

void ufbxt_check_string(ufbx_string str)
{
	// Data may never be NULL, empty strings should have data = ""
	ufbxt_assert(str.data != NULL);
	ufbxt_assert(strlen(str.data) == str.length);
}

void ufbxt_check_vertex_element(ufbx_scene *scene, ufbx_mesh *mesh, void *void_elem, size_t elem_size)
{
	ufbx_vertex_void *elem = (ufbx_vertex_void*)void_elem;
	if (elem->data == NULL) {
		ufbxt_assert(elem->indices == NULL);
		ufbxt_assert(elem->num_elements == 0);
		return;
	}

	ufbxt_assert(elem->num_elements >= 0);
	ufbxt_assert(elem->indices != NULL);

	// Check that the indices are in range
	for (size_t i = 0; i < mesh->num_indices; i++) {
		int32_t ix = elem->indices[i];
		ufbxt_assert(ix >= -1 && ix < elem->num_elements);
	}

	// Check that the data at invalid index is valid and zero
	char zero[32] = { 0 };
	ufbxt_assert(elem_size <= 32);
	ufbxt_assert(!memcmp((char*)elem->data - elem_size, zero, elem_size));
}

void ufbxt_check_props(ufbx_scene *scene, ufbx_props *props, bool top)
{
	ufbx_prop *prev = NULL;
	for (size_t i = 0; i < props->num_props; i++) {
		ufbx_prop *prop = &props->props[i];

		ufbxt_assert(prop->type < UFBX_NUM_PROP_TYPES);
		ufbxt_check_string(prop->name);
		ufbxt_check_string(prop->value_str);

		// Properties should be sorted by name and duplicates should be removed
		if (prev) {
			ufbxt_assert(prop->imp_key >= prev->imp_key);
			ufbxt_assert(strcmp(prop->name.data, prev->name.data) > 0);
		}

		if (top) {
			ufbx_prop *ref = ufbx_find_prop(props, prop->name.data);
			ufbxt_assert(prop == ref);
		}

		prev = prop;
	}

	if (props->defaults) {
		ufbxt_check_props(scene, props->defaults, false);
	}
}

void ufbxt_check_node(ufbx_scene *scene, ufbx_node *node)
{
	ufbxt_check_string(node->name);
	ufbxt_check_props(scene, &node->props, true);

	if (node->parent) {
		bool found = false;
		for (size_t i = 0; i < node->parent->children.size; i++) {
			if (node->parent->children.data[i] == node) {
				found = true;
				break;
			}
		}
		ufbxt_assert(found);
	}

	for (size_t i = 0; i < node->children.size; i++) {
		ufbxt_assert(node->children.data[i]->parent == node);
	}
}

void ufbxt_check_mesh(ufbx_scene *scene, ufbx_mesh *mesh)
{
	ufbx_mesh *found = ufbx_find_mesh(scene, mesh->node.name.data);
	ufbxt_assert(found && !strcmp(found->node.name.data, mesh->node.name.data));

	ufbxt_check_vertex_element(scene, mesh, &mesh->vertex_position, sizeof(ufbx_vec3));
	ufbxt_check_vertex_element(scene, mesh, &mesh->vertex_normal, sizeof(ufbx_vec3));
	ufbxt_check_vertex_element(scene, mesh, &mesh->vertex_binormal, sizeof(ufbx_vec3));
	ufbxt_check_vertex_element(scene, mesh, &mesh->vertex_tangent, sizeof(ufbx_vec3));
	ufbxt_check_vertex_element(scene, mesh, &mesh->vertex_uv, sizeof(ufbx_vec2));
	ufbxt_check_vertex_element(scene, mesh, &mesh->vertex_color, sizeof(ufbx_vec4));

	ufbxt_assert(mesh->num_vertices == mesh->vertex_position.num_elements);
	ufbxt_assert(mesh->num_triangles <= mesh->num_indices);

	uint32_t prev_end = 0;
	for (size_t i = 0; i < mesh->num_faces; i++) {
		ufbx_face face = mesh->faces[i];
		ufbxt_assert(face.index_begin == prev_end);
		ufbxt_assert(face.num_indices > 0);
		prev_end = face.index_begin + face.num_indices;
		ufbxt_assert(prev_end <= mesh->num_indices);

		for (size_t j = face.index_begin; j < face.index_begin + face.num_indices; j++) {
			ufbx_face *p_face = ufbx_find_face(mesh, j);
			ufbxt_assert(p_face - mesh->faces == i);
		}
	}

	for (size_t i = 0; i < mesh->num_edges; i++) {
		ufbx_edge edge = mesh->edges[i];
		ufbxt_assert(edge.indices[0] < mesh->num_indices);
		ufbxt_assert(edge.indices[1] < mesh->num_indices);
	}

	for (size_t i = 0; i < mesh->uv_sets.size; i++) {
		ufbx_uv_set *set = &mesh->uv_sets.data[i];
		if (i == 0) {
			ufbxt_assert(mesh->vertex_uv.data == set->vertex_uv.data);
			ufbxt_assert(mesh->vertex_uv.indices == set->vertex_uv.indices);
			ufbxt_assert(mesh->vertex_uv.num_elements == set->vertex_uv.num_elements);
		}
		ufbxt_check_string(set->name);
		ufbxt_check_vertex_element(scene, mesh, &set->vertex_uv, sizeof(ufbx_vec2));
	}

	for (size_t i = 0; i < mesh->color_sets.size; i++) {
		ufbx_color_set *set = &mesh->color_sets.data[i];
		if (i == 0) {
			ufbxt_assert(mesh->vertex_color.data == set->vertex_color.data);
			ufbxt_assert(mesh->vertex_color.indices == set->vertex_color.indices);
			ufbxt_assert(mesh->vertex_color.num_elements == set->vertex_color.num_elements);
		}
		ufbxt_check_string(set->name);
		ufbxt_check_vertex_element(scene, mesh, &set->vertex_color, sizeof(ufbx_vec4));
	}

	for (size_t i = 0; i < mesh->num_edges; i++) {
		ufbx_edge edge = mesh->edges[i];
		ufbxt_assert(edge.indices[0] < mesh->num_indices);
		ufbxt_assert(edge.indices[1] < mesh->num_indices);
		ufbx_face *face = ufbx_find_face(mesh, edge.indices[0]);
		ufbxt_assert(face);
		ufbxt_assert(face == ufbx_find_face(mesh, edge.indices[1]));
	}

	if (mesh->face_material) {
		for (size_t i = 0; i < mesh->num_faces; i++) {
			int32_t material = mesh->face_material[i];
			ufbxt_assert(material >= 0 && material < mesh->materials.size);
		}
	}
}

void ufbxt_check_material(ufbx_scene *scene, ufbx_material *material)
{
	ufbxt_check_string(material->name);
	ufbxt_check_props(scene, &material->props, true);
}

void ufbxt_check_scene(ufbx_scene *scene)
{
	ufbxt_check_string(scene->metadata.creator);

	for (size_t i = 0; i < scene->nodes.size; i++) {
		ufbxt_check_node(scene, scene->nodes.data[i]);
	}

	for (size_t i = 0; i < scene->meshes.size; i++) {
		ufbxt_check_mesh(scene, &scene->meshes.data[i]);
	}

	for (size_t i = 0; i < scene->materials.size; i++) {
		ufbxt_check_material(scene, &scene->materials.data[i]);
	}
}

static uint32_t g_file_version = 0;
static const char *g_file_type = NULL;
static bool g_fuzz = false;
static bool g_all_byte_values = false;
static bool g_dedicated_allocs = false;
static bool g_no_patch = false;
static int g_patch_start = 0;
static size_t g_fuzz_step = 0;

const char *g_fuzz_test_name = NULL;
uint16_t g_fuzz_test_version = 0;

static bool ufbxt_begin_fuzz()
{
	if (g_fuzz) {
		if (!g_skip_print_ok) {
			printf("FUZZ\n");
			g_skip_print_ok = true;
		}
		return true;
	} else {
		return false;
	}
}

int ufbxt_test_fuzz(void *data, size_t size, size_t step, int offset, size_t temp_limit, size_t result_limit, size_t truncate_length)
{
	if (g_fuzz_step && step != g_fuzz_step) return 1;

	t_jmp_buf = (jmp_buf*)calloc(1, sizeof(jmp_buf));
	int ret = 1;
	if (!setjmp(*t_jmp_buf)) {

		ufbx_load_opts opts = { 0 };
		opts.max_temp_allocs = temp_limit;
		opts.max_result_allocs = result_limit;

		if (g_dedicated_allocs) {
			opts.temp_huge_size = 1;
			opts.result_huge_size = 1;
		}

		if (truncate_length > 0) size = truncate_length;

		ufbx_error error;
		ufbx_scene *scene = ufbx_load_memory(data, size, &opts, &error);
		if (scene) {
			ufbxt_check_scene(scene);
			ufbx_free_scene(scene);

			ufbxt_assert(temp_limit == 0);
			ufbxt_assert(result_limit == 0);

		} else {

			// Collect hit checks
			for (size_t i = 0; i < error.stack_size; i++) {
				ufbx_error_frame frame = error.stack[i];
				ufbxt_check_line *check = &g_checks[frame.source_line];
				if (check->test_name && strcmp(g_fuzz_test_name, check->test_name)) continue;
				if (check->test_version && check->test_version < g_fuzz_test_version) continue;
				if ((uint32_t)offset > check->patch_offset - 1) continue;

				#pragma omp critical(check)
				{
					bool ok = (uint32_t)offset <= check->patch_offset - 1;
					if (check->test_name && strcmp(g_fuzz_test_name, check->test_name)) ok = false;
					if (check->test_version && check->test_version < g_fuzz_test_version) ok = false;

					if (ok) {
						check->test_name = g_fuzz_test_name;
						check->test_version = g_fuzz_test_version;
						if (offset < 0) {
							check->patch_offset = UINT32_MAX;
							check->patch_value = 0;
						} else {
							check->patch_offset = offset + 1;
							check->patch_value = ((uint8_t*)data)[offset];
						}
						check->temp_limit = (uint32_t)temp_limit;
						check->result_limit = (uint32_t)result_limit;
						check->truncate_length = (uint32_t)truncate_length;
						check->description = frame.description;
					}
				}
			}
		}

	} else {
		ret = 0;
	}

	free(t_jmp_buf);
	t_jmp_buf = NULL;

	return ret;

}

typedef struct {
	const char *name;
	uint16_t version;
	int32_t patch_offset;
	uint8_t patch_value;
	uint32_t temp_limit;
	uint32_t result_limit;
	uint32_t truncate_length;
	const char *description;
} ufbxt_fuzz_check;

// Generated by running `runner --fuzz`
static const ufbxt_fuzz_check g_fuzz_checks[] = {
        { "blender_279_default", 6102, -1, 0, 0, 0, 1, "header" },
        { "blender_279_default", 6102, -1, 0, 0, 0, 1, "uc->read_fn" },
        { "blender_279_default", 6102, -1, 0, 0, 0, 11775, "end == token->str_data + token->str_len - 1" },
        { "blender_279_default", 6102, -1, 0, 0, 0, 17, "header" },
        { "blender_279_default", 6102, -1, 0, 0, 0, 17, "uc->read_fn" },
        { "blender_279_default", 6102, -1, 0, 0, 0, 251, "depth == 0" },
        { "blender_279_default", 6102, -1, 0, 0, 0, 481, "c != '\\0'" },
        { "blender_279_default", 6102, -1, 0, 0, 0, 74417, "ufbxi_get_val1(node, \"S\", &layer->name)" },
        { "blender_279_default", 6102, -1, 0, 0, 1, 0, "defs" },
        { "blender_279_default", 6102, -1, 0, 0, 1, 0, "defs" },
        { "blender_279_default", 6102, -1, 0, 0, 128, 0, "name" },
        { "blender_279_default", 6102, -1, 0, 0, 149, 0, "ufbxi_push_string_place_str(uc, name)" },
        { "blender_279_default", 6102, -1, 0, 0, 149, 0, "ufbxi_split_type_and_name(uc, type_and_name, &type_str,..." },
        { "blender_279_default", 6102, -1, 0, 0, 16, 0, "dst" },
        { "blender_279_default", 6102, -1, 0, 0, 160, 0, "ufbxi_push_string_place_str(uc, &v->s)" },
        { "blender_279_default", 6102, -1, 0, 0, 180, 0, "ufbxi_push_string_place_str(uc, &prop->value_str)" },
        { "blender_279_default", 6102, -1, 0, 0, 224, 0, "name" },
        { "blender_279_default", 6102, -1, 0, 0, 240, 0, "ufbxi_push_string_place_str(uc, &v->s)" },
        { "blender_279_default", 6102, -1, 0, 0, 272, 0, "v" },
        { "blender_279_default", 6102, -1, 0, 0, 273, 0, "ufbxi_push_string_place_str(uc, name)" },
        { "blender_279_default", 6102, -1, 0, 0, 3, 0, "defs->props" },
        { "blender_279_default", 6102, -1, 0, 0, 3, 0, "defs->props" },
        { "blender_279_default", 6102, -1, 0, 0, 376, 0, "mesh->faces" },
        { "blender_279_default", 6102, -1, 0, 0, 416, 0, "dst->props" },
        { "blender_279_default", 6102, -1, 0, 0, 448, 0, "curve->keyframes.data" },
        { "blender_279_default", 6102, -1, 0, 0, 468, 0, "arr->data" },
        { "blender_279_default", 6102, -1, 0, 0, 468, 0, "ufbxi_retain_array(uc, sizeof(ufbx_model), &uc->scene.m..." },
        { "blender_279_default", 6102, -1, 0, 0, 469, 0, "ufbxi_retain_array(uc, sizeof(ufbx_mesh), &uc->scene.me..." },
        { "blender_279_default", 6102, -1, 0, 0, 470, 0, "ufbxi_retain_array(uc, sizeof(ufbx_light), &uc->scene.l..." },
        { "blender_279_default", 6102, -1, 0, 0, 471, 0, "ufbxi_retain_array(uc, sizeof(ufbx_anim_layer), &uc->sc..." },
        { "blender_279_default", 6102, -1, 0, 0, 472, 0, "ufbxi_retain_array(uc, sizeof(ufbx_anim_prop), &uc->sce..." },
        { "blender_279_default", 6102, -1, 0, 0, 473, 0, "ufbxi_retain_array(uc, sizeof(ufbx_anim_curve), &uc->sc..." },
        { "blender_279_default", 6102, -1, 0, 0, 474, 0, "zero_indices && consecutive_indices" },
        { "blender_279_default", 6102, -1, 0, 0, 475, 0, "ufbxi_merge_attribute_properties(uc, &parent.node->prop..." },
        { "blender_279_default", 6102, -1, 0, 0, 475, 0, "ufbxi_merge_properties(uc, props, props, attr, &uc->res..." },
        { "blender_279_default", 6102, -1, 0, 0, 476, 0, "layer->props.data" },
        { "blender_279_default", 6102, -1, 0, 0, 477, 0, "nodes" },
        { "blender_279_default", 6102, -1, 0, 0, 478, 0, "node->children.data" },
        { "blender_279_default", 6102, -1, 0, 0, 478, 0, "ufbxi_collect_nodes(uc, sizeof(ufbx_model), &nodes, uc-..." },
        { "blender_279_default", 6102, -1, 0, 0, 480, 0, "v" },
        { "blender_279_default", 6102, -1, 0, 0, 584, 0, "mesh->faces" },
        { "blender_279_default", 6102, -1, 0, 0, 64, 0, "dst" },
        { "blender_279_default", 6102, -1, 0, 0, 656, 0, "dst->props" },
        { "blender_279_default", 6102, -1, 0, 0, 720, 0, "curve->keyframes.data" },
        { "blender_279_default", 6102, -1, 0, 0, 726, 0, "arr->data" },
        { "blender_279_default", 6102, -1, 0, 0, 726, 0, "ufbxi_retain_array(uc, sizeof(ufbx_model), &uc->scene.m..." },
        { "blender_279_default", 6102, -1, 0, 0, 727, 0, "ufbxi_retain_array(uc, sizeof(ufbx_mesh), &uc->scene.me..." },
        { "blender_279_default", 6102, -1, 0, 0, 728, 0, "ufbxi_retain_array(uc, sizeof(ufbx_light), &uc->scene.l..." },
        { "blender_279_default", 6102, -1, 0, 0, 729, 0, "ufbxi_retain_array(uc, sizeof(ufbx_anim_layer), &uc->sc..." },
        { "blender_279_default", 6102, -1, 0, 0, 730, 0, "ufbxi_retain_array(uc, sizeof(ufbx_anim_prop), &uc->sce..." },
        { "blender_279_default", 6102, -1, 0, 0, 732, 0, "ufbxi_retain_array(uc, sizeof(ufbx_anim_curve), &uc->sc..." },
        { "blender_279_default", 6102, -1, 0, 0, 734, 0, "zero_indices && consecutive_indices" },
        { "blender_279_default", 6102, -1, 0, 0, 736, 0, "ufbxi_merge_attribute_properties(uc, &parent.node->prop..." },
        { "blender_279_default", 6102, -1, 0, 0, 736, 0, "ufbxi_merge_properties(uc, props, props, attr, &uc->res..." },
        { "blender_279_default", 6102, -1, 0, 0, 738, 0, "layer->props.data" },
        { "blender_279_default", 6102, -1, 0, 0, 740, 0, "nodes" },
        { "blender_279_default", 6102, -1, 0, 0, 742, 0, "node->children.data" },
        { "blender_279_default", 6102, -1, 0, 0, 742, 0, "ufbxi_collect_nodes(uc, sizeof(ufbx_model), &nodes, uc-..." },
        { "blender_279_default", 6102, -1, 0, 0, 744, 0, "imp" },
        { "blender_279_default", 6102, -1, 0, 0, 97, 0, "ufbxi_push_string_place_str(uc, &prop->value_str)" },
        { "blender_279_default", 6102, -1, 0, 1, 0, 0, "ufbxi_load_strings(uc)" },
        { "blender_279_default", 6102, -1, 0, 1, 0, 0, "ufbxi_load_strings(uc)" },
        { "blender_279_default", 6102, -1, 0, 1, 0, 0, "ufbxi_map_grow(&uc->string_map, ufbx_string, 16)" },
        { "blender_279_default", 6102, -1, 0, 1, 0, 0, "ufbxi_map_grow(&uc->string_map, ufbx_string, 16)" },
        { "blender_279_default", 6102, -1, 0, 1, 0, 0, "ufbxi_push_string_imp(uc, str->data, str->length, false..." },
        { "blender_279_default", 6102, -1, 0, 1, 0, 0, "ufbxi_push_string_imp(uc, str->data, str->length, false..." },
        { "blender_279_default", 6102, -1, 0, 1091, 0, 0, "light" },
        { "blender_279_default", 6102, -1, 0, 1091, 0, 0, "ufbxi_read_model(uc, node, &object)" },
        { "blender_279_default", 6102, -1, 0, 1266, 0, 0, "arr" },
        { "blender_279_default", 6102, -1, 0, 13, 0, 0, "ufbxi_ascii_next_token(uc, &uc->ascii.token)" },
        { "blender_279_default", 6102, -1, 0, 13, 0, 0, "ufbxi_ascii_push_token_char(uc, token, c)" },
        { "blender_279_default", 6102, -1, 0, 13, 0, 0, "ufbxi_ascii_push_token_char(uc, token, c)" },
        { "blender_279_default", 6102, -1, 0, 13, 0, 0, "ufbxi_begin_parse(uc)" },
        { "blender_279_default", 6102, -1, 0, 1380, 0, 0, "mesh" },
        { "blender_279_default", 6102, -1, 0, 1382, 0, 0, "mesh" },
        { "blender_279_default", 6102, -1, 0, 1382, 0, 0, "ufbxi_read_geometry(uc, node, &geom_obj)" },
        { "blender_279_default", 6102, -1, 0, 1384, 0, 0, "conn" },
        { "blender_279_default", 6102, -1, 0, 1384, 0, 0, "ufbxi_add_connection(uc, object->id, geom_obj.id, NULL)" },
        { "blender_279_default", 6102, -1, 0, 14, 0, 0, "model" },
        { "blender_279_default", 6102, -1, 0, 14, 0, 0, "model" },
        { "blender_279_default", 6102, -1, 0, 1433, 0, 0, "light" },
        { "blender_279_default", 6102, -1, 0, 1592, 0, 0, "arr" },
        { "blender_279_default", 6102, -1, 0, 16, 0, 0, "ufbxi_ascii_parse_node(uc, 0, UFBXI_PARSE_ROOT, &end, &..." },
        { "blender_279_default", 6102, -1, 0, 16, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_FBXHeaderExtension)" },
        { "blender_279_default", 6102, -1, 0, 160, 0, 0, "node->vals" },
        { "blender_279_default", 6102, -1, 0, 160, 0, 0, "ufbxi_ascii_parse_node(uc, 0, state, p_end, buf, true)" },
        { "blender_279_default", 6102, -1, 0, 160, 0, 0, "ufbxi_ascii_parse_node(uc, depth + 1, parse_state, &end..." },
        { "blender_279_default", 6102, -1, 0, 160, 0, 0, "ufbxi_parse_toplevel_child_imp(uc, state, &uc->tmp_pars..." },
        { "blender_279_default", 6102, -1, 0, 160, 0, 0, "ufbxi_parse_toplevel_child(uc, &node)" },
        { "blender_279_default", 6102, -1, 0, 160, 0, 0, "ufbxi_read_objects(uc)" },
        { "blender_279_default", 6102, -1, 0, 160, 0, 0, "ufbxi_read_root(uc)" },
        { "blender_279_default", 6102, -1, 0, 161, 0, 0, "node" },
        { "blender_279_default", 6102, -1, 0, 17, 0, 0, "ator->allocs_left > 1" },
        { "blender_279_default", 6102, -1, 0, 1722, 0, 0, "mesh" },
        { "blender_279_default", 6102, -1, 0, 1724, 0, 0, "mesh" },
        { "blender_279_default", 6102, -1, 0, 1726, 0, 0, "conn" },
        { "blender_279_default", 6102, -1, 0, 1726, 0, 0, "ufbxi_add_connection(uc, object->id, geom_obj.id, NULL)" },
        { "blender_279_default", 6102, -1, 0, 18, 0, 0, "ufbxi_grow_array(&uc->ator_tmp, &uc->top_nodes, &uc->to..." },
        { "blender_279_default", 6102, -1, 0, 18, 0, 0, "ufbxi_grow_array(&uc->ator_tmp, &uc->top_nodes, &uc->to..." },
        { "blender_279_default", 6102, -1, 0, 19, 0, 0, "ufbxi_ascii_accept(uc, UFBXI_ASCII_NAME)" },
        { "blender_279_default", 6102, -1, 0, 19, 0, 0, "ufbxi_ascii_next_token(uc, &ua->token)" },
        { "blender_279_default", 6102, -1, 0, 19, 0, 0, "ufbxi_ascii_push_token_char(uc, token, c)" },
        { "blender_279_default", 6102, -1, 0, 19, 0, 0, "ufbxi_ascii_push_token_char(uc, token, c)" },
        { "blender_279_default", 6102, -1, 0, 19, 0, 0, "ufbxi_grow_array(&uc->ator_tmp, &token->str_data, &toke..." },
        { "blender_279_default", 6102, -1, 0, 19, 0, 0, "ufbxi_grow_array(&uc->ator_tmp, &token->str_data, &toke..." },
        { "blender_279_default", 6102, -1, 0, 240, 0, 0, "node" },
        { "blender_279_default", 6102, -1, 0, 241, 0, 0, "ator->allocs_left > 1" },
        { "blender_279_default", 6102, -1, 0, 257, 0, 0, "prop" },
        { "blender_279_default", 6102, -1, 0, 257, 0, 0, "ufbxi_read_properties(uc, node, &object.props, &uc->res..." },
        { "blender_279_default", 6102, -1, 0, 329, 0, 0, "props->props" },
        { "blender_279_default", 6102, -1, 0, 331, 0, 0, "dst->props" },
        { "blender_279_default", 6102, -1, 0, 331, 0, 0, "ufbxi_merge_properties(uc, dst, &left, &right, buf)" },
        { "blender_279_default", 6102, -1, 0, 331, 0, 0, "ufbxi_sort_properties(uc, &left, &uc->tmp_sort)" },
        { "blender_279_default", 6102, -1, 0, 331, 0, 0, "ufbxi_sort_properties(uc, props, buf)" },
        { "blender_279_default", 6102, -1, 0, 333, 0, 0, "ufbxi_sort_properties(uc, &right, &uc->tmp_sort)" },
        { "blender_279_default", 6102, -1, 0, 4656, 0, 0, "ufbxi_parse_toplevel_child_imp(uc, state, &uc->tmp, &en..." },
        { "blender_279_default", 6102, -1, 0, 4656, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_Connections)" },
        { "blender_279_default", 6102, -1, 0, 4664, 0, 0, "node->children" },
        { "blender_279_default", 6102, -1, 0, 4672, 0, 0, "ufbxi_parse_toplevel_child(uc, &node)" },
        { "blender_279_default", 6102, -1, 0, 4672, 0, 0, "ufbxi_read_connections(uc)" },
        { "blender_279_default", 6102, -1, 0, 4674, 0, 0, "ufbxi_add_connection(uc, parent_id, child_id, prop)" },
        { "blender_279_default", 6102, -1, 0, 4678, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_Takes)" },
        { "blender_279_default", 6102, -1, 0, 4688, 0, 0, "ufbxi_parse_toplevel_child(uc, &node)" },
        { "blender_279_default", 6102, -1, 0, 4688, 0, 0, "ufbxi_read_takes(uc)" },
        { "blender_279_default", 6102, -1, 0, 4724, 0, 0, "v" },
        { "blender_279_default", 6102, -1, 0, 4725, 0, 0, "v" },
        { "blender_279_default", 6102, -1, 0, 4726, 0, 0, "v" },
        { "blender_279_default", 6102, -1, 0, 48, 0, 0, "node->children" },
        { "blender_279_default", 6102, -1, 0, 48, 0, 0, "ufbxi_parse_toplevel_child(uc, &child)" },
        { "blender_279_default", 6102, -1, 0, 48, 0, 0, "ufbxi_read_header_extension(uc)" },
        { "blender_279_default", 6102, -1, 0, 4800, 0, 0, "arr_data" },
        { "blender_279_default", 6102, -1, 0, 49, 0, 0, "ufbxi_add_connectable(uc, UFBXI_CONNECTABLE_MODEL, root..." },
        { "blender_279_default", 6102, -1, 0, 49, 0, 0, "ufbxi_map_grow(&uc->connectable_map, ufbxi_connectable,..." },
        { "blender_279_default", 6102, -1, 0, 496, 0, 0, "prop" },
        { "blender_279_default", 6102, -1, 0, 50, 0, 0, "data" },
        { "blender_279_default", 6102, -1, 0, 5020, 0, 0, "node->children" },
        { "blender_279_default", 6102, -1, 0, 5025, 0, 0, "ufbxi_add_connection(uc, parent_id, child_id, prop)" },
        { "blender_279_default", 6102, -1, 0, 51, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_Definitions)" },
        { "blender_279_default", 6102, -1, 0, 5104, 0, 0, "v" },
        { "blender_279_default", 6102, -1, 0, 5105, 0, 0, "arr_data" },
        { "blender_279_default", 6102, -1, 0, 5141, 0, 0, "v" },
        { "blender_279_default", 6102, -1, 0, 5142, 0, 0, "v" },
        { "blender_279_default", 6102, -1, 0, 5218, 0, 0, "layer" },
        { "blender_279_default", 6102, -1, 0, 5232, 0, 0, "ufbxi_add_connection(uc, layer_id, id, name)" },
        { "blender_279_default", 6102, -1, 0, 5248, 0, 0, "ufbxi_add_connection(uc, parent_id, id, name)" },
        { "blender_279_default", 6102, -1, 0, 5248, 0, 0, "ufbxi_read_take_anim_channel(uc, channel_nodes[i], id, ..." },
        { "blender_279_default", 6102, -1, 0, 5248, 0, 0, "ufbxi_read_take_object(uc, child, layer_id)" },
        { "blender_279_default", 6102, -1, 0, 5248, 0, 0, "ufbxi_read_take_prop_channel(uc, child, node_id, layer_..." },
        { "blender_279_default", 6102, -1, 0, 5248, 0, 0, "ufbxi_read_take_prop_channel(uc, child, node_id, layer_..." },
        { "blender_279_default", 6102, -1, 0, 5248, 0, 0, "ufbxi_read_take(uc, node)" },
        { "blender_279_default", 6102, -1, 0, 5249, 0, 0, "prop" },
        { "blender_279_default", 6102, -1, 0, 5296, 0, 0, "ufbxi_add_connection(uc, node_id, id, name)" },
        { "blender_279_default", 6102, -1, 0, 5297, 0, 0, "curve" },
        { "blender_279_default", 6102, -1, 0, 5303, 0, 0, "conns" },
        { "blender_279_default", 6102, -1, 0, 5303, 0, 0, "ufbxi_finalize_scene(uc)" },
        { "blender_279_default", 6102, -1, 0, 54, 0, 0, "ator->allocs_left > 1" },
        { "blender_279_default", 6102, -1, 0, 556, 0, 0, "props->props" },
        { "blender_279_default", 6102, -1, 0, 560, 0, 0, "dst->props" },
        { "blender_279_default", 6102, -1, 0, 560, 0, 0, "ufbxi_merge_properties(uc, dst, &left, &right, buf)" },
        { "blender_279_default", 6102, -1, 0, 560, 0, 0, "ufbxi_sort_properties(uc, &left, &uc->tmp_sort)" },
        { "blender_279_default", 6102, -1, 0, 560, 0, 0, "ufbxi_sort_properties(uc, props, buf)" },
        { "blender_279_default", 6102, -1, 0, 561, 0, 0, "ufbxi_sort_properties(uc, &right, &uc->tmp_sort)" },
        { "blender_279_default", 6102, -1, 0, 5645, 0, 0, "layer" },
        { "blender_279_default", 6102, -1, 0, 5648, 0, 0, "prop" },
        { "blender_279_default", 6102, -1, 0, 5649, 0, 0, "ufbxi_add_connection(uc, layer_id, id, name)" },
        { "blender_279_default", 6102, -1, 0, 5696, 0, 0, "ufbxi_add_connection(uc, parent_id, id, name)" },
        { "blender_279_default", 6102, -1, 0, 5697, 0, 0, "curve" },
        { "blender_279_default", 6102, -1, 0, 5760, 0, 0, "ufbxi_add_connection(uc, node_id, id, name)" },
        { "blender_279_default", 6102, -1, 0, 5809, 0, 0, "conns" },
        { "blender_279_default", 6102, -1, 0, 5809, 0, 0, "ufbxi_finalize_scene(uc)" },
        { "blender_279_default", 6102, -1, 0, 64, 0, 0, "node->children" },
        { "blender_279_default", 6102, -1, 0, 64, 0, 0, "ufbxi_parse_toplevel_child(uc, &object)" },
        { "blender_279_default", 6102, -1, 0, 64, 0, 0, "ufbxi_read_definitions(uc)" },
        { "blender_279_default", 6102, -1, 0, 65, 0, 0, "ufbxi_add_connectable(uc, UFBXI_CONNECTABLE_MODEL, root..." },
        { "blender_279_default", 6102, -1, 0, 65, 0, 0, "ufbxi_map_grow(&uc->connectable_map, ufbxi_connectable,..." },
        { "blender_279_default", 6102, -1, 0, 66, 0, 0, "data" },
        { "blender_279_default", 6102, -1, 0, 7, 0, 0, "ufbxi_load_maps(uc)" },
        { "blender_279_default", 6102, -1, 0, 7, 0, 0, "ufbxi_load_maps(uc)" },
        { "blender_279_default", 6102, -1, 0, 7, 0, 0, "ufbxi_map_grow(&uc->prop_type_map, ufbxi_prop_type_name..." },
        { "blender_279_default", 6102, -1, 0, 7, 0, 0, "ufbxi_map_grow(&uc->prop_type_map, ufbxi_prop_type_name..." },
        { "blender_279_default", 6102, -1, 0, 70, 0, 0, "ator->allocs_left > 1" },
        { "blender_279_default", 6102, -1, 0, 88, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_Objects)" },
        { "blender_279_default", 6102, -1, 0, 9, 0, 0, "str" },
        { "blender_279_default", 6102, -1, 0, 9, 0, 0, "str" },
        { "blender_279_default", 6102, -1, 0, 9, 0, 0, "ufbxi_load_default_props(uc)" },
        { "blender_279_default", 6102, -1, 0, 9, 0, 0, "ufbxi_load_default_props(uc)" },
        { "blender_279_default", 6102, -1, 0, 9, 0, 0, "ufbxi_push_string_place_str(uc, &prop->name)" },
        { "blender_279_default", 6102, -1, 0, 9, 0, 0, "ufbxi_push_string_place_str(uc, &prop->name)" },
        { "blender_279_default", 6102, -1, 0, 96, 0, 0, "node->vals" },
        { "blender_279_default", 6102, 0, 125, 0, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_Documents)" },
        { "blender_279_default", 6102, 0, 255, 0, 0, 0, "ufbxi_ascii_accept(uc, UFBXI_ASCII_NAME)" },
        { "blender_279_default", 6102, 0, 255, 0, 0, 0, "ufbxi_ascii_parse_node(uc, 0, UFBXI_PARSE_ROOT, &end, &..." },
        { "blender_279_default", 6102, 0, 255, 0, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_FBXHeaderExtension)" },
        { "blender_279_default", 6102, 0, 255, 0, 0, 0, "ufbxi_read_root(uc)" },
        { "blender_279_default", 6102, 0, 45, 0, 0, 0, "end == token->str_data + token->str_len - 1" },
        { "blender_279_default", 6102, 0, 46, 0, 0, 0, "end == token->str_data + token->str_len - 1" },
        { "blender_279_default", 6102, 0, 46, 0, 0, 0, "ufbxi_ascii_next_token(uc, &uc->ascii.token)" },
        { "blender_279_default", 6102, 0, 46, 0, 0, 0, "ufbxi_begin_parse(uc)" },
        { "blender_279_default", 6102, 1011, 0, 0, 0, 0, "ufbxi_check_string(*type)" },
        { "blender_279_default", 6102, 1011, 0, 0, 0, 0, "ufbxi_split_type_and_name(uc, type_and_name, &type_str,..." },
        { "blender_279_default", 6102, 103, 125, 0, 0, 0, "ufbxi_ascii_next_token(uc, &ua->token)" },
        { "blender_279_default", 6102, 1096, 58, 0, 0, 0, "ufbxi_get_val2(node, \"SC\", &prop->name, (char**)&type..." },
        { "blender_279_default", 6102, 1096, 58, 0, 0, 0, "ufbxi_read_properties(uc, node, &object.props, &uc->res..." },
        { "blender_279_default", 6102, 1096, 58, 0, 0, 0, "ufbxi_read_property(uc, prop_node, prop, version)" },
        { "blender_279_default", 6102, 180, 255, 0, 0, 0, "ufbxi_ascii_parse_node(uc, 0, state, p_end, buf, true)" },
        { "blender_279_default", 6102, 180, 255, 0, 0, 0, "ufbxi_parse_toplevel_child_imp(uc, state, &uc->tmp_pars..." },
        { "blender_279_default", 6102, 180, 255, 0, 0, 0, "ufbxi_parse_toplevel_child(uc, &child)" },
        { "blender_279_default", 6102, 180, 255, 0, 0, 0, "ufbxi_read_header_extension(uc)" },
        { "blender_279_default", 6102, 180, 42, 0, 0, 0, "ufbxi_ascii_accept(uc, UFBXI_ASCII_INT)" },
        { "blender_279_default", 6102, 180, 46, 0, 0, 0, "ufbxi_ascii_next_token(uc, &ua->token)" },
        { "blender_279_default", 6102, 203, 123, 0, 0, 0, "ufbxi_ascii_parse_node(uc, depth + 1, parse_state, &end..." },
        { "blender_279_default", 6102, 20377, 125, 0, 0, 0, "ufbxi_read_geometry(uc, node, &geom_obj)" },
        { "blender_279_default", 6102, 20377, 125, 0, 0, 0, "ufbxi_read_model(uc, node, &object)" },
        { "blender_279_default", 6102, 20377, 125, 0, 0, 0, "vertices && indices" },
        { "blender_279_default", 6102, 20377, 59, 0, 0, 0, "ix < mesh->num_vertices" },
        { "blender_279_default", 6102, 20379, 44, 0, 0, 0, "vertices->size % 3 == 0" },
        { "blender_279_default", 6102, 20644, 125, 0, 0, 0, "index_data[mesh->num_indices - 1] < 0" },
        { "blender_279_default", 6102, 20743, 125, 0, 0, 0, "data" },
        { "blender_279_default", 6102, 20743, 125, 0, 0, 0, "ufbxi_read_vertex_element(uc, mesh, n, &mesh->vertex_no..." },
        { "blender_279_default", 6102, 20743, 42, 0, 0, 0, "ufbxi_ascii_accept(uc, '}')" },
        { "blender_279_default", 6102, 20778, 59, 0, 0, 0, "ufbxi_find_val1(node, ufbxi_MappingInformationType, \"C..." },
        { "blender_279_default", 6102, 20807, 255, 0, 0, 0, "Invalid mapping" },
        { "blender_279_default", 6102, 20877, 44, 0, 0, 0, "data->size % num_components == 0" },
        { "blender_279_default", 6102, 251, 0, 0, 0, 0, "depth == 0" },
        { "blender_279_default", 6102, 26, 125, 0, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_Definitions)" },
        { "blender_279_default", 6102, 382, 0, 0, 0, 0, "c != '\\0'" },
        { "blender_279_default", 6102, 469, 123, 0, 0, 0, "ufbxi_parse_toplevel_child_imp(uc, state, &uc->tmp, &en..." },
        { "blender_279_default", 6102, 469, 125, 0, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_Objects)" },
        { "blender_279_default", 6102, 645, 255, 0, 0, 0, "ufbxi_parse_toplevel_child(uc, &object)" },
        { "blender_279_default", 6102, 645, 255, 0, 0, 0, "ufbxi_read_definitions(uc)" },
        { "blender_279_default", 6102, 73304, 125, 0, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_Takes)" },
        { "blender_279_default", 6102, 74083, 255, 0, 0, 0, "ufbxi_parse_toplevel_child(uc, &node)" },
        { "blender_279_default", 6102, 74083, 255, 0, 0, 0, "ufbxi_read_connections(uc)" },
        { "blender_279_default", 6102, 74100, 80, 0, 0, 0, "ufbxi_get_val_at(node, 3, 'C', (char**)&prop)" },
        { "blender_279_default", 6102, 74380, 255, 0, 0, 0, "ufbxi_parse_toplevel_child(uc, &node)" },
        { "blender_279_default", 6102, 74380, 255, 0, 0, 0, "ufbxi_read_takes(uc)" },
        { "blender_279_default", 6102, 74417, 255, 0, 0, 0, "ufbxi_get_val1(node, \"S\", &layer->name)" },
        { "blender_279_default", 6102, 74417, 255, 0, 0, 0, "ufbxi_read_take(uc, node)" },
        { "blender_279_default", 6102, 74618, 125, 0, 0, 0, "ufbxi_get_val1(node, \"c\", (char**)&type_and_name)" },
        { "blender_279_default", 6102, 74618, 125, 0, 0, 0, "ufbxi_read_take_object(uc, child, layer_id)" },
        { "blender_279_default", 6102, 74692, 59, 0, 0, 0, "ufbxi_get_val1(child, \"C\", (char**)&old_name)" },
        { "blender_279_default", 6102, 74692, 59, 0, 0, 0, "ufbxi_read_take_prop_channel(uc, child, node_id, layer_..." },
        { "blender_279_default", 6102, 74764, 58, 0, 0, 0, "data_end - data >= 2" },
        { "blender_279_default", 6102, 74764, 58, 0, 0, 0, "ufbxi_read_take_anim_channel(uc, channel_nodes[i], id, ..." },
        { "blender_279_default", 6102, 74764, 58, 0, 0, 0, "ufbxi_read_take_prop_channel(uc, child, node_id, layer_..." },
        { "blender_279_default", 6102, 74774, 59, 0, 0, 0, "ufbxi_find_val1(node, ufbxi_KeyCount, \"Z\", &num_keys)" },
        { "blender_279_default", 6102, 74790, 122, 0, 0, 0, "data_end - data >= 2" },
        { "blender_279_default", 6102, 74790, 46, 0, 0, 0, "data == data_end" },
        { "blender_279_default", 6102, 74815, 44, 0, 0, 0, "Unknown key mode" },
        { "blender_279_default", 6102, 74824, 57, 0, 0, 0, "data_end - data >= 3" },
        { "blender_279_default", 6102, 74843, 67, 0, 0, 0, "data_end - data >= 1" },
        { "blender_279_default", 6102, 74843, 85, 0, 0, 0, "data_end - data >= 4" },
        { "blender_279_default", 6102, 897, 125, 0, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_Connections)" },
        { "blender_279_default", 6102, 997, 255, 0, 0, 0, "ufbxi_parse_toplevel_child(uc, &node)" },
        { "blender_279_default", 6102, 997, 255, 0, 0, 0, "ufbxi_read_objects(uc)" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 129, "header" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 209, "data" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 21081, "data" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 21137, "res == (ptrdiff_t)decoded_data_size" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 21217, "data" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 21233, "uc->read_fn" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 21233, "uc->read_fn" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 21233, "ufbxi_read_to(uc, decoded_data, encoded_size)" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 21233, "ufbxi_read_to(uc, decoded_data, encoded_size)" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 21947, "ufbxi_read_bytes(uc, (size_t)to_skip)" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 21947, "ufbxi_skip_bytes(uc, encoded_size)" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 49, "name" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 497, "str || length == 0" },
        { "blender_279_default", 7401, -1, 0, 0, 0, 81, "name" },
        { "blender_279_default", 7401, -1, 0, 0, 130, 0, "name" },
        { "blender_279_default", 7401, -1, 0, 0, 224, 0, "ufbxi_push_string_place_str(uc, &vals[i].s)" },
        { "blender_279_default", 7401, -1, 0, 0, 240, 0, "name" },
        { "blender_279_default", 7401, -1, 0, 0, 331, 0, "edges" },
        { "blender_279_default", 7401, -1, 0, 0, 353, 0, "ufbxi_merge_attribute_properties(uc, &parent.node->prop..." },
        { "blender_279_default", 7401, -1, 0, 0, 608, 0, "edges" },
        { "blender_279_default", 7401, -1, 0, 1004, 0, 0, "ufbxi_grow_array(&uc->ator_tmp, &uc->read_buffer, &uc->..." },
        { "blender_279_default", 7401, -1, 0, 1008, 0, 0, "arr" },
        { "blender_279_default", 7401, -1, 0, 1009, 0, 0, "arr_data" },
        { "blender_279_default", 7401, -1, 0, 1009, 0, 0, "data" },
        { "blender_279_default", 7401, -1, 0, 1054, 0, 0, "ufbxi_read_geometry(uc, node, &object)" },
        { "blender_279_default", 7401, -1, 0, 1218, 0, 0, "uc->attributes" },
        { "blender_279_default", 7401, -1, 0, 1242, 0, 0, "uc->geometries" },
        { "blender_279_default", 7401, -1, 0, 1243, 0, 0, "ufbxi_merge_properties(uc, props, props->defaults, prop..." },
        { "blender_279_default", 7401, -1, 0, 133, 0, 0, "node->children" },
        { "blender_279_default", 7401, -1, 0, 1343, 0, 0, "uc->templates" },
        { "blender_279_default", 7401, -1, 0, 1383, 0, 0, "attr" },
        { "blender_279_default", 7401, -1, 0, 1383, 0, 0, "ufbxi_read_node_attribute(uc, node, &object)" },
        { "blender_279_default", 7401, -1, 0, 145, 0, 0, "ufbxi_parse_toplevel_child(uc, &child)" },
        { "blender_279_default", 7401, -1, 0, 145, 0, 0, "ufbxi_read_document(uc)" },
        { "blender_279_default", 7401, -1, 0, 1456, 0, 0, "ufbxi_push_string_place_str(uc, type)" },
        { "blender_279_default", 7401, -1, 0, 1492, 0, 0, "ufbxi_grow_array(&uc->ator_tmp, &uc->read_buffer, &uc->..." },
        { "blender_279_default", 7401, -1, 0, 1509, 0, 0, "arr" },
        { "blender_279_default", 7401, -1, 0, 158, 0, 0, "ufbxi_add_connectable(uc, UFBXI_CONNECTABLE_MODEL, root..." },
        { "blender_279_default", 7401, -1, 0, 16, 0, 0, "node" },
        { "blender_279_default", 7401, -1, 0, 16, 0, 0, "ufbxi_binary_parse_node(uc, 0, UFBXI_PARSE_ROOT, &end, ..." },
        { "blender_279_default", 7401, -1, 0, 1711, 0, 0, "uc->attributes" },
        { "blender_279_default", 7401, -1, 0, 1743, 0, 0, "uc->geometries" },
        { "blender_279_default", 7401, -1, 0, 1744, 0, 0, "ufbxi_merge_properties(uc, props, props->defaults, prop..." },
        { "blender_279_default", 7401, -1, 0, 1746, 0, 0, "ufbxi_merge_attribute_properties(uc, &parent.node->prop..." },
        { "blender_279_default", 7401, -1, 0, 220, 0, 0, "ufbxi_add_connectable(uc, UFBXI_CONNECTABLE_MODEL, root..." },
        { "blender_279_default", 7401, -1, 0, 32, 0, 0, "vals" },
        { "blender_279_default", 7401, -1, 0, 395, 0, 0, "tmpl" },
        { "blender_279_default", 7401, -1, 0, 448, 0, 0, "ufbxi_read_properties(uc, props, &tmpl->props, &uc->tmp..." },
        { "blender_279_default", 7401, -1, 0, 48, 0, 0, "vals" },
        { "blender_279_default", 7401, -1, 0, 644, 0, 0, "tmpl" },
        { "blender_279_default", 7401, -1, 0, 66, 0, 0, "node->children" },
        { "blender_279_default", 7401, -1, 0, 80, 0, 0, "node" },
        { "blender_279_default", 7401, -1, 0, 80, 0, 0, "ufbxi_binary_parse_node(uc, 0, state, p_end, buf, true)" },
        { "blender_279_default", 7401, -1, 0, 80, 0, 0, "ufbxi_binary_parse_node(uc, depth + 1, parse_state, &en..." },
        { "blender_279_default", 7401, -1, 0, 857, 0, 0, "uc->templates" },
        { "blender_279_default", 7401, -1, 0, 885, 0, 0, "ufbxi_read_properties(uc, node, &object.props, &uc->tmp..." },
        { "blender_279_default", 7401, -1, 0, 896, 0, 0, "attr" },
        { "blender_279_default", 7401, -1, 0, 896, 0, 0, "ufbxi_read_node_attribute(uc, node, &object)" },
        { "blender_279_default", 7401, -1, 0, 96, 0, 0, "ufbxi_parse_toplevel(uc, ufbxi_Documents)" },
        { "blender_279_default", 7401, -1, 0, 969, 0, 0, "ufbxi_push_string_place_str(uc, type)" },
        { "blender_279_default", 7401, 18187, 0, 0, 0, 0, "ufbxi_check_string(*name)" },
        { "blender_279_default", 7401, 18278, 2, 0, 0, 0, "ufbxi_read_properties(uc, node, &object.props, &uc->tmp..." },
        { "blender_279_default", 7401, 20939, 226, 0, 0, 0, "ufbxi_read_geometry(uc, node, &object)" },
        { "blender_279_default", 7401, 21081, 105, 0, 0, 0, "res == (ptrdiff_t)decoded_data_size" },
        { "blender_279_default", 7401, 21081, 255, 0, 0, 0, "Bad multivalue array type" },
        { "blender_279_default", 7401, 21081, 255, 0, 0, 0, "ufbxi_binary_parse_multivalue_array(uc, dst_type, arr_d..." },
        { "blender_279_default", 7401, 21085, 15, 0, 0, 0, "arr_data" },
        { "blender_279_default", 7401, 21085, 15, 0, 0, 0, "data" },
        { "blender_279_default", 7401, 21085, 15, 0, 0, 0, "size <= UFBXI_MAX_ALLOCATION_SIZE" },
        { "blender_279_default", 7401, 21085, 255, 0, 0, 0, "size <= uc->opts.max_array_size" },
        { "blender_279_default", 7401, 21086, 0, 0, 0, 0, "encoded_size == decoded_data_size" },
        { "blender_279_default", 7401, 21086, 255, 0, 0, 0, "Bad array encoding" },
        { "blender_279_default", 7401, 21209, 255, 0, 0, 0, "Bad multivalue array type" },
        { "blender_279_default", 7401, 21349, 255, 0, 0, 0, "index_ix >= 0 && (size_t)index_ix < mesh->num_indices" },
        { "blender_279_default", 7401, 24, 255, 0, 0, 0, "num_values64 <= (uint64_t)uc->opts.max_node_values" },
        { "blender_279_default", 7401, 24, 255, 0, 0, 0, "ufbxi_binary_parse_node(uc, 0, UFBXI_PARSE_ROOT, &end, ..." },
        { "blender_279_default", 7401, 25400, 2, 0, 0, 0, "header" },
        { "blender_279_default", 7401, 25553, 68, 0, 0, 0, "depth < uc->opts.max_child_depth" },
        { "blender_279_default", 7401, 25553, 68, 0, 0, 0, "ufbxi_check_node_depth(uc, *p_child, depth + 1)" },
        { "blender_279_default", 7401, 31, 255, 0, 0, 0, "Bad values type" },
        { "blender_279_default", 7401, 3210, 249, 0, 0, 0, "ufbxi_parse_toplevel_child(uc, &child)" },
        { "blender_279_default", 7401, 3210, 249, 0, 0, 0, "ufbxi_read_document(uc)" },
        { "blender_279_default", 7401, 35, 233, 0, 0, 0, "ufbxi_read_bytes(uc, (size_t)to_skip)" },
        { "blender_279_default", 7401, 35, 233, 0, 0, 0, "ufbxi_skip_bytes(uc, encoded_size)" },
        { "blender_279_default", 7401, 35, 255, 0, 0, 0, "ufbxi_binary_parse_node(uc, 0, state, p_end, buf, true)" },
        { "blender_279_default", 7401, 35, 5, 0, 0, 0, "str || length == 0" },
        { "blender_279_default", 7401, 35, 5, 0, 0, 0, "ufbxi_push_string_place_str(uc, &vals[i].s)" },
        { "blender_279_default", 7401, 35, 93, 0, 0, 0, "ufbxi_binary_parse_node(uc, depth + 1, parse_state, &en..." },
        { "blender_279_default", 7401, 35, 95, 0, 0, 0, "ufbxi_skip_bytes(uc, values_end_offset - offset)" },
        { "blender_279_default", 7401, 3698, 0, 0, 0, 0, "ufbxi_get_val1(object, \"C\", (char**)&tmpl->type)" },
        { "blender_279_default", 7401, 3762, 0, 0, 0, 0, "ufbxi_get_val1(props, \"S\", &tmpl->sub_type)" },
        { "blender_279_default", 7401, 3830, 2, 0, 0, 0, "ufbxi_get_val_at(node, val_ix++, 'C', (char**)&subtype_..." },
        { "blender_279_default", 7401, 3830, 2, 0, 0, 0, "ufbxi_read_properties(uc, props, &tmpl->props, &uc->tmp..." },
        { "blender_279_default", 7401, 58, 255, 0, 0, 0, "current_offset == end_offset" },
        { "blender_279_default", 7401, 66, 4, 0, 0, 0, "offset <= values_end_offset" },
        { "blender_279_default", 7401, 822, 98, 0, 0, 0, "data" },
        { "blender_282_suzanne", 7401, -1, 0, 0, 211, 0, "mesh->uv_sets.data" },
        { "blender_282_suzanne", 7401, -1, 0, 0, 379, 0, "mesh->uv_sets.data" },
        { "blender_282_suzanne", 7401, 22882, 167, 0, 0, 0, "ufbxi_read_vertex_element(uc, mesh, n, &set->vertex_uv...." },
        { "blender_282_suzanne", 7401, 22994, 14, 0, 0, 0, "Invalid mapping" },
        { "maya_auto_clamp", 7102, -1, 0, 1120, 0, 0, "v" },
        { "maya_auto_clamp", 7102, -1, 0, 1122, 0, 0, "v" },
        { "maya_auto_clamp", 7102, -1, 0, 816, 0, 0, "v" },
        { "maya_auto_clamp", 7102, -1, 0, 817, 0, 0, "v" },
        { "maya_color_sets", 6101, -1, 0, 0, 193, 0, "mesh->color_sets.data" },
        { "maya_color_sets", 6101, -1, 0, 0, 347, 0, "mesh->color_sets.data" },
        { "maya_color_sets", 6101, 9881, 189, 0, 0, 0, "ufbxi_read_vertex_element(uc, mesh, n, &set->vertex_col..." },
        { "maya_cube", 6101, -1, 0, 0, 0, 10731, "ufbxi_skip_bytes(uc, values_end_offset - offset)" },
        { "maya_cube", 6101, -1, 0, 0, 0, 6929, "ufbxi_binary_parse_multivalue_array(uc, dst_type, arr_d..." },
        { "maya_cube", 6101, -1, 0, 0, 0, 6929, "val" },
        { "maya_cube", 6101, -1, 0, 0, 0, 7185, "val" },
        { "maya_cube", 6101, -1, 0, 273, 0, 0, "arr_data" },
        { "maya_cube", 6101, 6766, 15, 0, 0, 0, "arr_data" },
        { "maya_cube", 6101, 8133, 159, 0, 0, 0, "ufbxi_read_vertex_element(uc, mesh, n, &mesh->vertex_bi..." },
        { "maya_cube", 6101, 9008, 159, 0, 0, 0, "ufbxi_read_vertex_element(uc, mesh, n, &mesh->vertex_ta..." },
        { "maya_cube", 7101, -1, 0, 1114, 0, 0, "layer" },
        { "maya_cube", 7101, -1, 0, 1114, 0, 0, "ufbxi_read_animation_layer(uc, node, &object)" },
        { "maya_cube", 7101, -1, 0, 804, 0, 0, "layer" },
        { "maya_cube", 7101, -1, 0, 804, 0, 0, "ufbxi_read_animation_layer(uc, node, &object)" },
        { "maya_cube", 7102, -1, 0, 0, 0, 8909, "vertices && indices" },
        { "maya_cube", 7102, -1, 0, 0, 0, 8929, "ufbxi_ascii_accept(uc, UFBXI_ASCII_NAME)" },
        { "maya_cube", 7102, -1, 0, 0, 0, 8993, "ufbxi_ascii_accept(uc, '}')" },
        { "maya_cube", 7102, -1, 0, 0, 0, 9152, "ufbxi_ascii_accept(uc, UFBXI_ASCII_INT)" },
        { "maya_cube", 7102, 8926, 123, 0, 0, 0, "ufbxi_ascii_accept(uc, UFBXI_ASCII_NAME)" },
        { "maya_cube", 7102, 8935, 42, 0, 0, 0, "!in_ascii_array" },
        { "maya_interpolation_modes", 6101, 16936, 50, 0, 0, 0, "data_end - data >= 2" },
        { "maya_interpolation_modes", 7501, -1, 0, 0, 265, 0, "keys" },
        { "maya_interpolation_modes", 7501, -1, 0, 0, 477, 0, "keys" },
        { "maya_interpolation_modes", 7501, -1, 0, 1171, 0, 0, "curve" },
        { "maya_interpolation_modes", 7501, -1, 0, 1190, 0, 0, "prop" },
        { "maya_interpolation_modes", 7501, -1, 0, 1190, 0, 0, "ufbxi_read_animation_curve_node(uc, node, &object)" },
        { "maya_interpolation_modes", 7501, -1, 0, 855, 0, 0, "curve" },
        { "maya_interpolation_modes", 7501, -1, 0, 855, 0, 0, "ufbxi_read_animation_curve(uc, node, &object)" },
        { "maya_interpolation_modes", 7501, -1, 0, 891, 0, 0, "prop" },
        { "maya_interpolation_modes", 7501, -1, 0, 891, 0, 0, "ufbxi_read_animation_curve_node(uc, node, &object)" },
        { "maya_interpolation_modes", 7501, 24256, 248, 0, 0, 0, "times = ufbxi_find_array(node, ufbxi_KeyTime, 'l')" },
        { "maya_interpolation_modes", 7501, 24256, 248, 0, 0, 0, "ufbxi_read_animation_curve(uc, node, &object)" },
        { "maya_interpolation_modes", 7501, 24387, 255, 0, 0, 0, "values = ufbxi_find_array(node, ufbxi_KeyValueFloat, 'r..." },
        { "maya_interpolation_modes", 7501, 24418, 255, 0, 0, 0, "Bad multivalue array type" },
        { "maya_interpolation_modes", 7501, 24418, 89, 0, 0, 0, "times->size == values->size" },
        { "maya_interpolation_modes", 7501, 24527, 148, 0, 0, 0, "attr_flags = ufbxi_find_array(node, ufbxi_KeyAttrFlags,..." },
        { "maya_interpolation_modes", 7501, 24627, 255, 0, 0, 0, "attrs = ufbxi_find_array(node, ufbxi_KeyAttrDataFloat, ..." },
        { "maya_interpolation_modes", 7501, 24663, 89, 0, 0, 0, "attr_flags->size == refs->size" },
        { "maya_interpolation_modes", 7501, 24724, 255, 0, 0, 0, "refs = ufbxi_find_array(node, ufbxi_KeyAttrRefCount, 'i..." },
        { "maya_interpolation_modes", 7501, 24765, 255, 0, 0, 0, "Bad multivalue array type" },
        { "maya_interpolation_modes", 7501, 24765, 89, 0, 0, 0, "attrs->size == refs->size * 4u" },
        { "maya_interpolation_modes", 7501, 25010, 102, 0, 0, 0, "refs_left >= 0" },
        { "maya_interpolation_modes", 7502, -1, 0, 0, 0, 13315, "times = ufbxi_find_array(node, ufbxi_KeyTime, 'l')" },
        { "maya_interpolation_modes", 7502, -1, 0, 0, 0, 17377, "ufbxi_get_val_at(node, 3, 'C', (char**)&prop)" },
        { "maya_interpolation_modes", 7502, -1, 0, 1168, 0, 0, "v" },
        { "maya_interpolation_modes", 7502, -1, 0, 845, 0, 0, "v" },
        { "maya_rotation_order", 6102, -1, 0, 0, 0, 5318, "end == token->str_data + token->str_len - 1" },
};

void ufbxt_do_file_test(const char *name, void (*test_fn)(ufbx_scene *s, ufbxt_diff_error *err))
{
	const uint32_t file_versions[] = { 6100, 7100, 7400, 7500, 7700 };

	char buf[512];
	snprintf(buf, sizeof(buf), "%s%s.obj", data_root, name);
	size_t obj_size = 0;
	void *obj_data = ufbxt_read_file(buf, &obj_size);
	ufbxt_obj_file *obj_file = obj_data ? ufbxt_load_obj(obj_data, obj_size) : NULL;
	free(obj_data);

	ufbxt_begin_fuzz();

	uint32_t num_opened = 0;

	for (uint32_t vi = 0; vi < ufbxt_arraycount(file_versions); vi++) {
		for (uint32_t fi = 0; fi < 2; fi++) {
			uint32_t version = file_versions[vi];
			const char *format = fi == 1 ? "ascii" : "binary";
			snprintf(buf, sizeof(buf), "%s%s_%u_%s.fbx", data_root, name, version, format);

			if (g_file_version && version != g_file_version) continue;
			if (g_file_type && strcmp(format, g_file_type)) continue;

			size_t size = 0;
			void *data = ufbxt_read_file(buf, &size);
			if (!data) continue;

			num_opened++;
			ufbxt_logf("%s", buf);


			ufbx_error error;

			ufbx_load_opts load_opts = { 0 };
			if (g_dedicated_allocs) {
				load_opts.temp_huge_size = 1;
				load_opts.result_huge_size = 1;
			}

			uint64_t load_begin = cputime_cpu_tick();
			ufbx_scene *scene = ufbx_load_memory(data, size, &load_opts, &error);
			uint64_t load_end = cputime_cpu_tick();

			if (!scene) {
				ufbxt_log_error(&error);
				ufbxt_assert_fail(__FILE__, __LINE__, "Failed to parse file");
			}

			ufbx_load_opts stream_opts = load_opts;
			stream_opts.read_buffer_size = 1;
			ufbx_scene *streamed_scene = ufbx_load_file(buf, &stream_opts, &error);
			if (!streamed_scene) {
				ufbxt_log_error(&error);
				ufbxt_assert_fail(__FILE__, __LINE__, "Failed to parse streamed file");
			}
			ufbx_free_scene(streamed_scene);

			ufbxt_logf(".. Loaded in %.2fms: File %.1fkB, temp %.1fkB (%zu allocs), result %.1fkB (%zu allocs)",
				cputime_cpu_delta_to_sec(NULL, load_end - load_begin) * 1e3,
				(double)size * 1e-3,
				(double)scene->metadata.temp_memory_used * 1e-3,
				scene->metadata.temp_allocs,
				(double)scene->metadata.result_memory_used * 1e-3,
				scene->metadata.result_allocs
			);

			ufbxt_assert(scene->metadata.ascii == ((fi == 1) ? 1 : 0));
			ufbxt_assert(scene->metadata.version == version);

			ufbxt_check_scene(scene);

			ufbxt_diff_error err = { 0 };

			if (obj_file) {
				ufbxt_diff_to_obj(scene, obj_file, &err);
			}

			test_fn(scene, &err);

			if (err.num > 0) {
				ufbx_real avg = err.sum / (ufbx_real)err.num;
				ufbxt_logf(".. Absolute diff: avg %.3g, max %.3g (%zu tests)", avg, err.max, err.num);
			}

			size_t temp_allocs = scene->metadata.temp_allocs;
			size_t result_allocs = scene->metadata.result_allocs;

			ufbx_free_scene(scene);

			uint16_t packed_version = (uint16_t)(version + fi + 1);
			if (g_fuzz) {
				size_t step = 0;

				size_t fail_step = 0;
				int i;

				g_fuzz_test_name = name;
				g_fuzz_test_version = packed_version;

				#pragma omp parallel for schedule(static, 16)
				for (i = 0; i < (int)temp_allocs; i++) {
					if (omp_get_thread_num() == 0) {
						if (i % 16 == 0) {
							fprintf(stderr, "\rFuzzing temp limit %s_%u_%s: %d/%d", name, version, format, i, (int)temp_allocs);
							fflush(stderr);
						}
					}

					size_t step = 10000000 + (size_t)i;

					if (!ufbxt_test_fuzz(data, size, step, -1, (size_t)i, 0, 0)) fail_step = step;
				}

				fprintf(stderr, "\rFuzzing temp limit %s_%u_%s: %d/%d\n", name, version, format, (int)temp_allocs, (int)temp_allocs);

				#pragma omp parallel for schedule(static, 16)
				for (i = 0; i < (int)result_allocs; i++) {
					if (omp_get_thread_num() == 0) {
						if (i % 16 == 0) {
							fprintf(stderr, "\rFuzzing result limit %s_%u_%s: %d/%d", name, version, format, i, (int)result_allocs);
							fflush(stderr);
						}
					}

					size_t step = 20000000 + (size_t)i;

					if (!ufbxt_test_fuzz(data, size, step, -1, 0, (size_t)i, 0)) fail_step = step;
				}

				fprintf(stderr, "\rFuzzing result limit %s_%u_%s: %d/%d\n", name, version, format, (int)result_allocs, (int)result_allocs);

				#pragma omp parallel for schedule(static, 16)
				for (i = 1; i < (int)size; i++) {
					if (omp_get_thread_num() == 0) {
						if (i % 16 == 0) {
							fprintf(stderr, "\rFuzzing truncate %s_%u_%s: %d/%d", name, version, format, i, (int)size);
							fflush(stderr);
						}
					}

					size_t step = 30000000 + (size_t)i;

					if (!ufbxt_test_fuzz(data, size, step, -1, 0, 0, (size_t)i)) fail_step = step;
				}

				fprintf(stderr, "\rFuzzing truncate %s_%u_%s: %d/%d\n", name, version, format, (int)size, (int)size);

				if (!g_no_patch) {

					uint8_t *data_copy[256] = { 0 };

					int patch_start = g_patch_start - omp_get_num_threads() * 16;
					if (patch_start < 0) {
						patch_start = 0;
					}

					#pragma omp parallel for schedule(static, 16)
					for (i = patch_start; i < (int)size; i++) {

						if (omp_get_thread_num() == 0) {
							if (i % 16 == 0) {
								fprintf(stderr, "\rFuzzing patch %s_%u_%s: %d/%d", name, version, format, i, (int)size);
								fflush(stderr);
							}
						}

						uint8_t **p_data_copy = &data_copy[omp_get_thread_num()];
						if (*p_data_copy == NULL) {
							*p_data_copy = malloc(size);
							memcpy(*p_data_copy, data, size);
						}
						uint8_t *data_u8 = *p_data_copy;

						size_t step = i * 1000;

						uint8_t original = data_u8[i];

						if (g_all_byte_values) {
							for (uint32_t v = 0; v < 256; v++) {
								data_u8[i] = (uint8_t)v;
								if (!ufbxt_test_fuzz(data_u8, size, step + v, i, 0, 0, 0)) fail_step = step + v;
							}
						} else {
							data_u8[i] = original + 1;
							if (!ufbxt_test_fuzz(data_u8, size, step + 1, i, 0, 0, 0)) fail_step = step + 1;

							data_u8[i] = original - 1;
							if (!ufbxt_test_fuzz(data_u8, size, step + 2, i, 0, 0, 0)) fail_step = step + 2;

							if (original != 0) {
								data_u8[i] = 0;
								if (!ufbxt_test_fuzz(data_u8, size, step + 3, i, 0, 0, 0)) fail_step = step + 3;
							}

							if (original != 0xff) {
								data_u8[i] = 0xff;
								if (!ufbxt_test_fuzz(data_u8, size, step + 4, i, 0, 0, 0)) fail_step = step + 4;
							}
						}


						data_u8[i] = original;
					}

					fprintf(stderr, "\rFuzzing patch %s_%u_%s: %d/%d\n", name, version, format, (int)size, (int)size);

					for (size_t i = 0; i < ufbxt_arraycount(data_copy); i++) {
						free(data_copy[i]);
					}

				}

				ufbxt_hintf("Fuzz failed on step: %zu", step);
				ufbxt_assert(fail_step == 0);

			} else {
				uint8_t *data_u8 = (uint8_t*)data;

				// Run a couple of known fuzz checks
				for (size_t i = 0; i < ufbxt_arraycount(g_fuzz_checks); i++) {
					const ufbxt_fuzz_check *check = &g_fuzz_checks[i];
					if (check->version != packed_version) continue;
					if (strcmp(check->name, name)) continue;

					uint8_t original;
					if (check->patch_offset >= 0) {
						original = data_u8[check->patch_offset];
						ufbxt_logf(".. Patch byte %u from 0x%02x to 0x%02x: %s", check->patch_offset, original, check->patch_value, check->description);
						ufbxt_assert(check->patch_offset < size);
						data_u8[check->patch_offset] = check->patch_value;
					}

					ufbx_load_opts opts = { 0 };

					if (check->temp_limit > 0) {
						ufbxt_logf(".. Temp limit %u: %s", check->temp_limit, check->description);
						opts.max_temp_allocs = check->temp_limit;
					}

					if (check->result_limit > 0) {
						ufbxt_logf(".. Result limit %u: %s", check->result_limit, check->description);
						opts.max_result_allocs = check->result_limit;
					}

					size_t truncated_size = size;
					if (check->truncate_length > 0) {
						ufbxt_logf(".. Truncated length %u: %s", check->truncate_length, check->description);
						truncated_size = check->truncate_length;
					}

					ufbx_error error;
					ufbx_scene *scene = ufbx_load_memory(data, truncated_size, &opts, &error);
					if (scene) {
						ufbxt_check_scene(scene);
						ufbx_free_scene(scene);
					}

					if (check->patch_offset >= 0) {
						data_u8[check->patch_offset] = original;
					}
				}

			}

			free(data);
		}
	}

	if (num_opened == 0) {
		ufbxt_assert_fail(__FILE__, __LINE__, "File not found");
	}

	free(obj_file);
}

static ufbx_real ufbxt_dot2(ufbx_vec2 a, ufbx_vec2 b)
{
	return a.x*b.x + a.y*b.y;
}

static ufbx_real ufbxt_dot3(ufbx_vec3 a, ufbx_vec3 b)
{
	return a.x*b.x + a.y*b.y + a.z*b.z;
}

static ufbx_vec2 ufbxt_add2(ufbx_vec2 a, ufbx_vec2 b)
{
	ufbx_vec2 v;
	v.x = a.x + b.x;
	v.y = a.y + b.y;
	return v;
}

static ufbx_vec3 ufbxt_add3(ufbx_vec3 a, ufbx_vec3 b)
{
	ufbx_vec3 v;
	v.x = a.x + b.x;
	v.y = a.y + b.y;
	v.z = a.z + b.z;
	return v;
}


static ufbx_vec2 ufbxt_sub2(ufbx_vec2 a, ufbx_vec2 b)
{
	ufbx_vec2 v;
	v.x = a.x - b.x;
	v.y = a.y - b.y;
	return v;
}

static ufbx_vec3 ufbxt_sub3(ufbx_vec3 a, ufbx_vec3 b)
{
	ufbx_vec3 v;
	v.x = a.x - b.x;
	v.y = a.y - b.y;
	v.z = a.z - b.z;
	return v;
}

#define UFBXT_IMPL 1
#define UFBXT_TEST(name) void ufbxt_test_fn_##name(void)
#define UFBXT_FILE_TEST(name) void ufbxt_test_fn_imp_file_##name(ufbx_scene *scene, ufbxt_diff_error *err); \
	void ufbxt_test_fn_file_##name(void) { \
	ufbxt_do_file_test(#name, &ufbxt_test_fn_imp_file_##name); } \
	void ufbxt_test_fn_imp_file_##name(ufbx_scene *scene, ufbxt_diff_error *err)

#include "all_tests.h"

#undef UFBXT_IMPL
#undef UFBXT_TEST
#undef UFBXT_FILE_TEST
#define UFBXT_IMPL 0
#define UFBXT_TEST(name) { #name, &ufbxt_test_fn_##name },
#define UFBXT_FILE_TEST(name) { #name, &ufbxt_test_fn_file_##name },
ufbxt_test g_tests[] = {
	#include "all_tests.h"
};

int ufbxt_run_test(ufbxt_test *test)
{
	printf("%s: ", test->name);
	fflush(stdout);

	g_error.stack_size = 0;
	g_hint[0] = '\0';

	g_current_test = test;
	if (!setjmp(g_test_jmp)) {
		g_skip_print_ok = false;
		test->func();
		if (!g_skip_print_ok) {
			printf("OK\n");
			fflush(stdout);
		}
		return 1;
	} else {
		if (g_hint[0]) {
			ufbxt_logf("Hint: %s", g_hint);
		}
		if (g_error.stack_size) {
			ufbxt_log_error(&g_error);
		}

		return 0;
	}
}

int main(int argc, char **argv)
{
	uint32_t num_tests = ufbxt_arraycount(g_tests);
	uint32_t num_ok = 0;
	const char *test_filter = NULL;

	cputime_init();

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			g_verbose = 1;
		}
		if (!strcmp(argv[i], "-t")) {
			if (++i < argc) {
				test_filter = argv[i];
			}
		}
		if (!strcmp(argv[i], "-d")) {
			if (++i < argc) {
				size_t len = strlen(argv[i]);
				if (len + 2 > sizeof(data_root)) {
					fprintf(stderr, "-d: Data root too long");
					return 1;
				}
				memcpy(data_root, argv[i], len);
				char end = argv[i][len - 1];
				if (end != '/' && end != '\\') {
					data_root[len] = '/';
					data_root[len + 1] = '\0';
				}
			}
		}
		if (!strcmp(argv[i], "-f")) {
			if (++i < argc) g_file_version = (uint32_t)atoi(argv[i]);
			if (++i < argc) g_file_type = argv[i];
		}

		if (!strcmp(argv[i], "--fuzz")) {
			g_fuzz = true;
		}

		if (!strcmp(argv[i], "--patch-all-byte-values")) {
			g_all_byte_values = true;
		}

		if (!strcmp(argv[i], "--patch-start")) {
			if (++i < argc) g_patch_start = atoi(argv[i]);
		}

		if (!strcmp(argv[i], "--dedicated-allocs")) {
			g_dedicated_allocs = true;
		}

		if (!strcmp(argv[i], "--no-patch")) {
			g_no_patch = true;
		}

		if (!strcmp(argv[i], "--threads")) {
			#if _OPENMP
			if (++i < argc) omp_set_num_threads(atoi(argv[i]));
			#endif
		}

		if (!strcmp(argv[i], "--fuzz-step")) {
			if (++i < argc) g_fuzz_step = (size_t)atoi(argv[i]);
		}
	}

	#ifdef _OPENMP
	if (omp_get_num_threads() > 256) {
		omp_set_num_threads(256);
	}
	#else
	if (g_fuzz) {
		fprintf(stderr, "Fuzzing without threads, compile with OpenMP for better performance!\n");
	}
	#endif

	uint32_t num_ran = 0;
	for (uint32_t i = 0; i < num_tests; i++) {
		ufbxt_test *test = &g_tests[i];
		if (test_filter && strcmp(test->name, test_filter)) {
			continue;
		}

		num_ran++;
		if (ufbxt_run_test(test)) {
			num_ok++;
		}

		ufbxt_log_flush();
	}

	if (num_ok < num_tests) {
		printf("\n");
		for (uint32_t i = 0; i < num_tests; i++) {
			ufbxt_test *test = &g_tests[i];
			if (test->fail.failed) {
				ufbxt_fail *fail = &test->fail;
				const char *file = fail->file, *find;
				find = strrchr(file, '/');
				file = find ? find + 1 : file;
				find = strrchr(file, '\\');
				file = find ? find + 1 : file;
				printf("(%s) %s:%u: %s\n", test->name,
					file, fail->line, fail->expr);
			}
		}
	}

	printf("\nTests passed: %u/%u\n", num_ok, num_ran);

	if (g_fuzz) {
		printf("Fuzz checks:\n\nstatic const ufbxt_fuzz_check g_fuzz_checks[] = {\n");
		for (size_t i = 0; i < ufbxt_arraycount(g_checks); i++) {
			ufbxt_check_line *check = &g_checks[i];
			if (check->patch_offset == 0) continue;

			char safe_desc[60];
			size_t safe_desc_len = 0;
			for (const char *c = check->description; *c; c++) {
				if (sizeof(safe_desc) - safe_desc_len < 6) {
					safe_desc[safe_desc_len++] = '.';
					safe_desc[safe_desc_len++] = '.';
					safe_desc[safe_desc_len++] = '.';
					break;
				}
				if (*c == '"' || *c == '\\') {
					safe_desc[safe_desc_len++] = '\\';
				}
				safe_desc[safe_desc_len++] = *c;
			}
			safe_desc[safe_desc_len] = '\0';

			int32_t patch_offset = check->patch_offset != UINT32_MAX ? (int32_t)(check->patch_offset - 1) : -1;

			printf("\t{ \"%s\", %u, %d, %u, %u, %u, %u, \"%s\" },\n", check->test_name, (uint32_t)check->test_version,
				patch_offset, (uint32_t)check->patch_value, (uint32_t)check->temp_limit, (uint32_t)check->result_limit, (uint32_t)check->truncate_length, safe_desc);
		}
		printf("};\n");
	}

	return num_ok == num_ran ? 0 : 1;
}
