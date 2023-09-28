#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

static void ufbxt_assert_fail_imp(const char *func, const char *file, size_t line, const char *msg)
{
	fprintf(stderr, "%s:%zu: %s(%s) failed\n", file, line, func, msg);
	exit(2);
}

#define ufbxt_assert_fail(file, line, msg) ufbxt_assert_fail_imp("ufbxt_assert_fail", file, line, msg)
#define ufbxt_assert(m_cond) do { if (!(m_cond)) ufbxt_assert_fail_imp("ufbxt_assert", __FILE__, __LINE__, #m_cond); } while (0)
#define ufbx_assert(m_cond) do { if (!(m_cond)) ufbxt_assert_fail_imp("ufbx_assert", __FILE__, __LINE__, #m_cond); } while (0)

bool g_verbose = false;

#include "../ufbx.h"
#include "check_scene.h"
#include "check_material.h"
#include "testing_utils.h"
#include "cputime.h"

typedef struct {
	const char *name;
	int value;
} ufbxt_enum_name;

static const ufbxt_enum_name ufbxt_names_ufbx_geometry_transform_handling[] = {
	{ "preserve", UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE },
	{ "helper-nodes", UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES },
	{ "modify-geometry", UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY },
	{ "modify-geometry-no-fallback", UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY_NO_FALLBACK },
};

static const ufbxt_enum_name ufbxt_names_ufbx_inherit_mode_handling[] = {
	{ "preserve", UFBX_INHERIT_MODE_HANDLING_PRESERVE },
	{ "helper-nodes", UFBX_INHERIT_MODE_HANDLING_HELPER_NODES },
	{ "compensate", UFBX_INHERIT_MODE_HANDLING_COMPENSATE },
};

static const ufbxt_enum_name ufbxt_names_ufbx_space_conversion[] = {
	{ "transform-root", UFBX_SPACE_CONVERSION_TRANSFORM_ROOT },
	{ "adjust-transforms", UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS },
};

static const ufbxt_enum_name ufbxt_names_ufbx_index_error_handling[] = {
	{ "clamp", UFBX_INDEX_ERROR_HANDLING_CLAMP },
	{ "no-index", UFBX_INDEX_ERROR_HANDLING_NO_INDEX },
	{ "abort", UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING },
	{ "unsafe-ignore", UFBX_INDEX_ERROR_HANDLING_UNSAFE_IGNORE },
};

static int ufbxt_str_to_enum_imp(const ufbxt_enum_name *names, size_t count, const char *type_name, const char *name)
{
	for (size_t i = 0; i < count; i++) {
		if (!strcmp(names[i].name, name)) {
			return names[i].value;
		}
	}
	fprintf(stderr, "Unsupported enum name in %s: %s\n", type_name, name);
	ufbxt_assert(false);
	return -1;
}

#define ufbxt_cat2(a, b) a##b
#define ufbxt_cat(a, b) ufbxt_cat2(a,b)
#define ufbxt_str_to_enum_names(m_type, m_names, m_str) \
	(m_type)ufbxt_str_to_enum_imp(m_names, ufbxt_arraycount(m_names), #m_type, (m_str))
#define ufbxt_str_to_enum(m_type, m_str) \
	ufbxt_str_to_enum_names(m_type, ufbxt_cat2(ufbxt_names_, m_type), m_str)

typedef struct {
	size_t count;
	const char *names[64];
} ufbxt_fbx_features;

static void ufbxt_add_feature(ufbxt_fbx_features *features, const char *name)
{
	for (size_t i = 0; i < features->count; i++) {
		if (!strcmp(features->names[i], name)) return;
	}

	size_t index = features->count++;
	ufbxt_assert(index < ufbxt_arraycount(features->names));
	features->names[index] = name;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv)
#else
int main(int argc, char **argv)
#endif
{
#ifdef _WIN32
	char **argv = (char**)malloc(sizeof(char*) * argc);
	ufbxt_assert(argv);
	for (int i = 0; i < argc; i++) {
		int res = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, NULL, 0, NULL, NULL);
		ufbxt_assert(res > 0);
		size_t dst_size = (size_t)res + 1;
		char *dst = (char*)malloc(dst_size);
		ufbxt_assert(dst);
		res = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, dst, (int)dst_size, NULL, NULL);
		ufbxt_assert(res > 0 && (size_t)res < dst_size);
		argv[i] = dst;
	}
#endif

	cputime_begin_init();

	const char *path = NULL;
	const char *obj_path = NULL;
	const char *mat_path = NULL;
	const char *dump_obj_path = NULL;
	int profile_runs = 0;
	int frame = INT_MIN;
	bool allow_bad_unicode = false;
	bool sink = false;
	bool dedicated_allocs = false;
	bool bake = false;
	double override_fps = -1.0;

	ufbx_load_opts opts = { 0 };

	opts.evaluate_skinning = true;
	opts.evaluate_caches = true;
	opts.load_external_files = true;
	opts.generate_missing_normals = true;
	opts.target_axes = ufbx_axes_right_handed_y_up;
	opts.target_unit_meters = (ufbx_real)0.01;
	opts.obj_search_mtl_by_filename = true;
	opts.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			g_verbose = true;
		} else if (!strcmp(argv[i], "--obj")) {
			if (++i < argc) obj_path = argv[i];
		} else if (!strcmp(argv[i], "--mat")) {
			if (++i < argc) mat_path = argv[i];
		} else if (!strcmp(argv[i], "--dump-obj")) {
			if (++i < argc) dump_obj_path = argv[i];
		} else if (!strcmp(argv[i], "--profile-runs")) {
			if (++i < argc) profile_runs = atoi(argv[i]);
		} else if (!strcmp(argv[i], "--frame")) {
			if (++i < argc) frame = atoi(argv[i]);
		} else if (!strcmp(argv[i], "--allow-bad-unicode")) {
			allow_bad_unicode = true;
		} else if (!strcmp(argv[i], "--dedicated-allocs")) {
			dedicated_allocs = true;
		} else if (!strcmp(argv[i], "--sink")) {
			sink = true;
		} else if (!strcmp(argv[i], "--ignore-missing-external")) {
			opts.ignore_missing_external_files = true;
		} else if (!strcmp(argv[i], "--geometry-transform-handling")) {
			if (++i < argc) opts.geometry_transform_handling = ufbxt_str_to_enum(ufbx_geometry_transform_handling, argv[i]);
		} else if (!strcmp(argv[i], "--inherit-mode-handling")) {
			if (++i < argc) opts.inherit_mode_handling = ufbxt_str_to_enum(ufbx_inherit_mode_handling, argv[i]);
		} else if (!strcmp(argv[i], "--space-conversion")) {
			if (++i < argc) opts.space_conversion = ufbxt_str_to_enum(ufbx_space_conversion, argv[i]);
		} else if (!strcmp(argv[i], "--index-error-handling")) {
			if (++i < argc) opts.index_error_handling = ufbxt_str_to_enum(ufbx_index_error_handling, argv[i]);
		} else if (!strcmp(argv[i], "--fps")) {
			if (++i < argc) override_fps = strtod(argv[i], NULL);
		} else if (!strcmp(argv[i], "--bake")) {
			bake = true;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Unrecognized flag: %s\n", argv[i]);
			exit(1);
		} else {
			path = argv[i];
		}
	}

	if (!path) {
		fprintf(stderr, "Usage: check_fbx <file.fbx>\n");
		return 1;
	}

	if (strstr(path, "ufbx-bad-unicode")) {
		allow_bad_unicode = true;
	}

	if (dedicated_allocs) {
		opts.temp_allocator.huge_threshold = 1;
		opts.result_allocator.huge_threshold = 1;
	}

	if (!allow_bad_unicode) {
		opts.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_ABORT_LOADING;
	}

	ufbx_error error;
	ufbx_scene *scene;

	uint64_t load_delta = 0;
	{
		uint64_t load_begin = cputime_cpu_tick();
		scene = ufbx_load_file(path, &opts, &error);
		uint64_t load_end = cputime_cpu_tick();
		load_delta = load_end - load_begin;
	}

	if (!scene) {
		char buf[1024];
		ufbx_format_error(buf, sizeof(buf), &error);
		fprintf(stderr, "%s\n", buf);
		return 1;
	}

	cputime_end_init();

	{
		size_t fbx_size = ufbxt_file_size(path);
		printf("Loaded in %.2fms: File %.1fkB, temp %.1fkB (%zu allocs), result %.1fkB (%zu allocs)\n",
			cputime_cpu_delta_to_sec(NULL, load_delta) * 1e3,
			(double)fbx_size * 1e-3,
			(double)scene->metadata.temp_memory_used * 1e-3,
			scene->metadata.temp_allocs,
			(double)scene->metadata.result_memory_used * 1e-3,
			scene->metadata.result_allocs
		);
	}

	const char *exporters[] = {
		"Unknown",
		"FBX SDK",
		"Blender Binary",
		"Blender ASCII",
		"MotionBuilder",
		"Unity Exporter (from Building Crafter)",
	};

	const char *formats[2][2] = {
		{ "binary", "binary (big-endian)" },
		{ "ascii", "!?!?ascii (big-endian)!?!?" },
	};

	const char *application = scene->metadata.latest_application.name.data;
	if (!application[0]) application = "unknown";

	printf("FBX %u %s via %s %u.%u.%u (%s)\n",
		scene->metadata.version,
		formats[scene->metadata.ascii][scene->metadata.big_endian],
		exporters[scene->metadata.exporter],
		ufbx_version_major(scene->metadata.exporter_version),
		ufbx_version_minor(scene->metadata.exporter_version),
		ufbx_version_patch(scene->metadata.exporter_version),
		application);

	{
		size_t fbx_size = 0;
		void *fbx_data = ufbxt_read_file(path, &fbx_size);
		if (fbx_data) {

			for (int i = 0; i < profile_runs; i++) {
				uint64_t load_begin = cputime_cpu_tick();
				ufbx_scene *memory_scene = ufbx_load_memory(fbx_data, fbx_size, NULL, NULL);
				uint64_t load_end = cputime_cpu_tick();

				printf("Loaded in %.2fms: File %.1fkB, temp %.1fkB (%zu allocs), result %.1fkB (%zu allocs)\n",
					cputime_cpu_delta_to_sec(NULL, load_end - load_begin) * 1e3,
					(double)fbx_size * 1e-3,
					(double)scene->metadata.temp_memory_used * 1e-3,
					scene->metadata.temp_allocs,
					(double)scene->metadata.result_memory_used * 1e-3,
					scene->metadata.result_allocs
				);

				ufbxt_assert(memory_scene);
				ufbx_free_scene(memory_scene);
			}

			free(fbx_data);
		}
	}

	int result = 0;

	if (!strstr(path, "ufbx-unknown")) {
		bool ignore_unknowns = false;
		bool has_unknowns = false;

		for (size_t i = 0; i < scene->unknowns.count; i++) {
			ufbx_unknown *unknown = scene->unknowns.data[i];
			if (strstr(unknown->super_type.data, "MotionBuilder")) continue;
			if (strstr(unknown->type.data, "Container")) continue;
			if (!strcmp(unknown->super_type.data, "Object") && unknown->type.length == 0 && unknown->sub_type.length == 0) continue;
			if (!strcmp(unknown->super_type.data, "PluginParameters")) continue;
			if (!strcmp(unknown->super_type.data, "TimelineXTrack")) continue;
			if (!strcmp(unknown->super_type.data, "GlobalShading")) continue;
			if (!strcmp(unknown->super_type.data, "ControlSetPlug")) continue;
			if (!strcmp(unknown->sub_type.data, "NodeAttribute")) continue;
			if (!strcmp(unknown->type.data, "GroupSelection")) continue;
			if (!strcmp(unknown->name.data, "ADSKAssetReferencesVersion3.0")) {
				ignore_unknowns = true;
			}

			has_unknowns = true;
			fprintf(stderr, "Unknown element: %s/%s/%s : %s\n", unknown->super_type.data, unknown->type.data, unknown->sub_type.data, unknown->name.data);
		}

		if (has_unknowns && !ignore_unknowns) {
			result = 3;
		}
	}

	bool known_unknown = false;
	if (strstr(scene->metadata.creator.data, "kenney")) known_unknown = true;
	if (strstr(scene->metadata.creator.data, "assetforge")) known_unknown = true;
	if (scene->metadata.version < 5800) known_unknown = true;
	ufbxt_assert(scene->metadata.exporter != UFBX_EXPORTER_UNKNOWN || known_unknown);

	ufbxt_check_scene(scene);

	ufbxt_fbx_features features = { 0 };
	for (size_t i = 0; i < scene->nodes.count; i++) {
		ufbx_node *node = scene->nodes.data[i];
		if (node->has_geometry_transform) {
			ufbxt_add_feature(&features, "geometry-transform");
		}
	}

	for (size_t i = 0; i < scene->meshes.count; i++) {
		ufbx_mesh *mesh = scene->meshes.data[i];
		if (mesh->instances.count > 1) {
			ufbxt_add_feature(&features, "instanced-mesh");
		}
	}

	if (scene->nurbs_curves.count > 0) ufbxt_add_feature(&features, "nurbs-curve");
	if (scene->nurbs_surfaces.count > 0) ufbxt_add_feature(&features, "nurbs-surface");
	if (scene->lod_groups.count > 0) ufbxt_add_feature(&features, "lod-group");

	if (features.count > 0) {
		printf("Features:");
		for (size_t i = 0; i < features.count; i++) {
			printf(" %s", features.names[i]);
		}
		printf("\n");
	}

	if (obj_path) {
		size_t obj_size;
		void *obj_data = ufbxt_read_file_ex(obj_path, &obj_size);
		if (!obj_data) {
			fprintf(stderr, "Failed to read .obj file: %s\n", obj_path);
			return 1;
		}

		ufbxt_load_obj_opts obj_opts = { 0 };

		ufbxt_obj_file *obj_file = ufbxt_load_obj(obj_data, obj_size, &obj_opts);

		obj_file->normalize_units = true;

		ufbx_scene *state;
		if (obj_file->animation_frame >= 0 || frame != INT_MIN) {
			ufbx_anim *anim = scene->anim;

			if (obj_file->animation_name[0]) {
				ufbx_anim_stack *stack = ufbx_find_anim_stack(scene, obj_file->animation_name);
				ufbxt_assert(stack);
				anim = stack->anim;
			}

			double fps = scene->settings.frames_per_second;
			if (override_fps > 0.0)
				fps = override_fps;

			double time = anim->time_begin + (double)obj_file->animation_frame / fps;

			if (frame != INT_MIN) {
				time = (double)frame / fps;
			}

			if (bake) {
				ufbx_bake_opts opts = { 0 };
				opts.max_keyframe_segments = 4096;

				ufbx_baked_anim *bake = ufbx_bake_anim(scene, anim, &opts, NULL);
				ufbxt_assert(bake);

				size_t num_prop_overrides = 0;
				for (size_t i = 0; i < bake->elements.count; i++) {
					num_prop_overrides += bake->elements.data[i].props.count;
				}

				ufbx_prop_override_desc *prop_overrides = (ufbx_prop_override_desc*)calloc(num_prop_overrides, sizeof(ufbx_prop_override_desc));
				ufbx_transform_override *transform_overrides = (ufbx_transform_override*)calloc(bake->nodes.count, sizeof(ufbx_transform_override));
				ufbxt_assert(prop_overrides);
				ufbxt_assert(transform_overrides);

				ufbx_prop_override_desc *prop_over = prop_overrides;
				for (size_t elem_ix = 0; elem_ix < bake->elements.count; elem_ix++) {
					ufbx_baked_element *elem = &bake->elements.data[elem_ix];
					for (size_t prop_ix = 0; prop_ix < elem->props.count; prop_ix++) {
						ufbx_baked_prop *prop = &elem->props.data[prop_ix];
						prop_over->element_id = elem->element_id;
						prop_over->prop_name = prop->name;
						ufbx_vec3 val = ufbx_evaluate_baked_vec3(prop->keys, time);
						prop_over->value.x = val.x;
						prop_over->value.y = val.y;
						prop_over->value.z = val.z;
						prop_over++;
					}
				}

				for (size_t i = 0; i < bake->nodes.count; i++) {
					ufbx_baked_node *node = &bake->nodes.data[i];
					transform_overrides[i].node_id = node->typed_id;
					transform_overrides[i].transform.translation = ufbx_evaluate_baked_vec3(node->translation_keys, time);
					transform_overrides[i].transform.rotation = ufbx_evaluate_baked_quat(node->rotation_keys, time);
					transform_overrides[i].transform.scale = ufbx_evaluate_baked_vec3(node->scale_keys, time);
				}

				ufbxt_assert(prop_over == prop_overrides + num_prop_overrides);

				ufbx_anim_opts anim_opts = { 0 };
				anim_opts.prop_overrides.data = prop_overrides;
				anim_opts.prop_overrides.count = num_prop_overrides;
				anim_opts.transform_overrides.data = transform_overrides;
				anim_opts.transform_overrides.count = bake->nodes.count;

				anim = ufbx_create_anim(scene, &anim_opts, NULL);
				ufbxt_assert(anim);
			}

			ufbx_evaluate_opts eval_opts = { 0 };
			eval_opts.evaluate_skinning = true;
			eval_opts.evaluate_caches = true;
			eval_opts.load_external_files = true;
			state = ufbx_evaluate_scene(scene, anim, time, &eval_opts, NULL);
			ufbxt_assert(state);

			ufbx_free_anim(anim);
		} else {
			state = scene;
			ufbx_retain_scene(state);
		}

		if (dump_obj_path) {
			ufbxt_debug_dump_obj_scene(dump_obj_path, state);
			printf("Dumped .obj to %s\n", dump_obj_path);
		}

		ufbxt_diff_error err = { 0 };

		uint32_t diff_flags = 0;

		if (bake) {
			diff_flags |= UFBXT_OBJ_DIFF_FLAG_BAKED_ANIM;
		}

		ufbxt_diff_to_obj(state, obj_file, &err, diff_flags);

		if (err.num > 0) {
			ufbx_real avg = err.sum / (ufbx_real)err.num;
			printf("Absolute diff: avg %.3g, max %.3g (%zu tests)\n", avg, err.max, err.num);
		}

		ufbx_free_scene(state);
		free(obj_file);
		free(obj_data);
	} else {
		if (dump_obj_path) {
			ufbxt_debug_dump_obj_scene(dump_obj_path, scene);
			printf("Dumped .obj to %s\n", dump_obj_path);
		}
	}

	if (mat_path) {
		size_t mat_size;
		void *mat_data = ufbxt_read_file_ex(mat_path, &mat_size);
		if (!mat_data) {
			fprintf(stderr, "Failed to read .mat file: %s\n", mat_path);
			return 1;
		}

		const char *mat_filename = mat_path + strlen(mat_path);
		while (mat_filename > mat_path && (mat_filename[-1] != '\\' && mat_filename[-1] != '/')) {
			mat_filename--;
		}
		bool ok = ufbxt_check_materials(scene, (const char*)mat_data, mat_filename);
		if (!ok && !result) {
			result = 4;
		}

		free(mat_data);
	}

	ufbx_free_scene(scene);

	if (sink) {
		printf("%u\n", ufbxt_sink);
	}

	return result;
}

#define CPUTIME_IMPLEMENTATION
#ifndef UFBX_DEV
	#define UFBX_DEV
#endif

#include "cputime.h"
#include "../ufbx.c"
