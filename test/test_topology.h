#undef UFBXT_TEST_GROUP
#define UFBXT_TEST_GROUP "topology"

#if UFBXT_IMPL
void ufbxt_check_generated_normals(ufbx_mesh *mesh, ufbxt_diff_error *err, size_t expected_normals)
{
	ufbx_topo_edge *topo = calloc(mesh->num_indices, sizeof(ufbx_topo_edge));
	ufbxt_assert(topo);

	ufbx_compute_topology(mesh, topo, mesh->num_indices);

	uint32_t *normal_indices = calloc(mesh->num_indices, sizeof(uint32_t));
	size_t num_normals = ufbx_generate_normal_mapping(mesh, topo, mesh->num_indices, normal_indices, mesh->num_indices, false);

	if (expected_normals > 0) {
		ufbxt_assert(num_normals == expected_normals);
	}

	ufbx_vec3 *normals = calloc(num_normals, sizeof(ufbx_vec3));
	ufbx_compute_normals(mesh, &mesh->vertex_position, normal_indices, mesh->num_indices, normals, num_normals);

	ufbx_vertex_vec3 new_normals = { 0 };
	new_normals.exists = true;
	new_normals.values.data = normals;
	new_normals.values.count = num_normals;
	new_normals.indices.data = normal_indices;
	new_normals.indices.count = mesh->num_indices;

	for (size_t i = 0; i < mesh->num_indices; i++) {
		ufbx_vec3 fn = ufbx_get_vertex_vec3(&mesh->vertex_normal, i);
		ufbx_vec3 rn = ufbx_get_vertex_vec3(&new_normals, i);

		ufbxt_assert_close_vec3(err, fn, rn);
	}

	free(normals);
	free(normal_indices);
	free(topo);
}
#endif

UFBXT_FILE_TEST(maya_edge_smoothing)
#if UFBXT_IMPL
{
	ufbx_node *node = ufbx_find_node(scene, "pCube1");
	ufbxt_assert(node && node->mesh);
	ufbx_mesh *mesh = node->mesh;
	ufbxt_check_generated_normals(mesh, err, 16);
}
#endif

UFBXT_FILE_TEST(maya_no_smoothing)
#if UFBXT_IMPL
{
	ufbx_node *node = ufbx_find_node(scene, "pCube1");
	ufbxt_assert(node && node->mesh);
	ufbx_mesh *mesh = node->mesh;
	ufbxt_check_generated_normals(mesh, err, 16);
}
#endif

UFBXT_FILE_TEST(maya_planar_ngon)
#if UFBXT_IMPL
{
	ufbx_node *node = ufbx_find_node(scene, "pDisc1");
	ufbxt_assert(node && node->mesh);
	ufbx_mesh *mesh = node->mesh;
	ufbxt_check_generated_normals(mesh, err, 0);
}
#endif

UFBXT_FILE_TEST(maya_subsurf_cube)
#if UFBXT_IMPL
{
}
#endif

UFBXT_FILE_TEST(maya_subsurf_plane)
#if UFBXT_IMPL
{
}
#endif

UFBXT_FILE_TEST(maya_subsurf_cube_crease)
#if UFBXT_IMPL
{
}
#endif

UFBXT_FILE_TEST(blender_293_suzanne_subsurf)
#if UFBXT_IMPL
{
}
#endif

UFBXT_FILE_TEST_ALT(subsurf_alloc_fail, maya_subsurf_cube)
#if UFBXT_IMPL
{
	ufbx_node *node = ufbx_find_node(scene, "pCube1");
	ufbxt_assert(node && node->mesh);
	ufbx_mesh *mesh = node->mesh;

	for (size_t max_temp = 1; max_temp < 10000; max_temp++) {
		ufbx_subdivide_opts opts = { 0 };
		opts.temp_allocator.huge_threshold = 1;
		opts.temp_allocator.allocation_limit = max_temp;

		ufbxt_hintf("Temp limit: %zu", max_temp);

		ufbx_error error;
		ufbx_mesh *sub_mesh = ufbx_subdivide_mesh(mesh, 2, &opts, &error);
		if (sub_mesh) {
			ufbxt_logf(".. Tested up to %zu temporary allocations", max_temp);
			ufbx_free_mesh(sub_mesh);
			break;
		}
		ufbxt_assert(error.type == UFBX_ERROR_ALLOCATION_LIMIT);
	}

	for (size_t max_result = 1; max_result < 10000; max_result++) {
		ufbx_subdivide_opts opts = { 0 };
		opts.result_allocator.huge_threshold = 1;
		opts.result_allocator.allocation_limit = max_result;

		ufbxt_hintf("Result limit: %zu", max_result);

		ufbx_error error;
		ufbx_mesh *sub_mesh = ufbx_subdivide_mesh(mesh, 2, &opts, &error);
		if (sub_mesh) {
			ufbxt_logf(".. Tested up to %zu result allocations", max_result);
			ufbx_free_mesh(sub_mesh);
			break;
		}
		ufbxt_assert(error.type == UFBX_ERROR_ALLOCATION_LIMIT);
	}
}
#endif

#if UFBXT_IMPL
typedef struct {
	const char *node_name;
	ufbx_subdivision_boundary boundary;
} ufbxt_subsurf_boundary_ref;
#endif

UFBXT_FILE_TEST(blender_293_suzanne_subsurf_uv)
#if UFBXT_IMPL
{
	ufbxt_subsurf_boundary_ref ref[] = {
		{ "SharpBoundary", UFBX_SUBDIVISION_BOUNDARY_SHARP_BOUNDARY },
		{ "SharpCorners", UFBX_SUBDIVISION_BOUNDARY_SHARP_CORNERS },
		{ "SharpInterior", UFBX_SUBDIVISION_BOUNDARY_SHARP_INTERIOR },
		{ "SharpNone", UFBX_SUBDIVISION_BOUNDARY_SHARP_NONE },
	};

	for (size_t i = 0; i < ufbxt_arraycount(ref); i++) {
		ufbx_node *node = ufbx_find_node(scene, ref[i].node_name);
		ufbxt_assert(node);

		ufbx_prop *prop = ufbx_find_prop(&node->props, "ufbx:UVBoundary");
		ufbxt_assert(prop);
		ufbxt_assert((prop->flags & UFBX_PROP_FLAG_USER_DEFINED) != 0);
		ufbxt_assert(prop->type == UFBX_PROP_INTEGER);
		ufbxt_assert(prop->value_int == (int64_t)ref[i].boundary);
	}
}
#endif

UFBXT_FILE_TEST(blender_293x_nonmanifold_subsurf)
#if UFBXT_IMPL
{
}
#endif

UFBXT_FILE_TEST(blender_293_ngon_subsurf)
#if UFBXT_IMPL
{
}
#endif

UFBXT_FILE_TEST(blender_293x_subsurf_boundary)
#if UFBXT_IMPL
{
}
#endif

UFBXT_FILE_TEST(blender_293x_subsurf_max_crease)
#if UFBXT_IMPL
{
	ufbx_mesh *mesh = (ufbx_mesh*)ufbx_find_element(scene, UFBX_ELEMENT_MESH, "Plane");
	ufbx_mesh *subdivided = ufbx_subdivide_mesh(mesh, 1, NULL, NULL);

	for (size_t i = 0; i < mesh->num_edges; i++) {
		ufbxt_assert_close_real(err, mesh->edge_crease.data[i], 1.0f);
	}

	size_t num_edge = 0;
	size_t num_center = 0;
	for (size_t i = 0; i < subdivided->num_edges; i++) {
		ufbx_edge edge = subdivided->edges.data[i];
		ufbx_vec3 a = ufbx_get_vertex_vec3(&subdivided->vertex_position, edge.a);
		ufbx_vec3 b = ufbx_get_vertex_vec3(&subdivided->vertex_position, edge.b);
		ufbx_real a_len = a.x*a.x + a.y*a.y + a.z*a.z;
		ufbx_real b_len = b.x*b.x + b.y*b.y + b.z*b.z;

		if (a_len < 0.01f || b_len < 0.01f) {
			ufbxt_assert_close_real(err, subdivided->edge_crease.data[i], 0.0f);
			num_center++;
		} else {
			ufbxt_assert_close_real(err, subdivided->edge_crease.data[i], 1.0f);
			num_edge++;
		}
	}

	ufbxt_assert(num_edge == 8);
	ufbxt_assert(num_center == 4);

	ufbx_free_mesh(subdivided);
}
#endif

UFBXT_FILE_TEST(maya_subsurf_max_crease)
#if UFBXT_IMPL
{
	ufbx_node *node = ufbx_find_node(scene, "pCube1");
	ufbxt_assert(node && node->mesh);
	ufbx_mesh *subdivided = ufbx_subdivide_mesh(node->mesh, 1, NULL, NULL);
	ufbxt_assert(subdivided);

	size_t num_top = 0;
	size_t num_bottom = 0;
	for (size_t i = 0; i < subdivided->num_edges; i++) {
		ufbx_edge edge = subdivided->edges.data[i];
		ufbx_vec3 a = ufbx_get_vertex_vec3(&subdivided->vertex_position, edge.a);
		ufbx_vec3 b = ufbx_get_vertex_vec3(&subdivided->vertex_position, edge.b);
		ufbx_real a_len = a.x*a.x + a.z*a.z;
		ufbx_real b_len = b.x*b.x + b.z*b.z;

		if (a.y < -0.49f && b.y < -0.49f && a_len > 0.01f && b_len > 0.01f) {
			ufbxt_assert_close_real(err, subdivided->edge_crease.data[i], 0.8f);
			num_bottom++;
		} else if (a.y > +0.49f && b.y > +0.49f && a_len > 0.01f && b_len > 0.01f) {
			ufbxt_assert_close_real(err, subdivided->edge_crease.data[i], 1.0f);
			num_top++;
		} else {
			ufbxt_assert_close_real(err, subdivided->edge_crease.data[i], 0.0f);
		}
		a = a;
	}

	ufbxt_assert(num_top == 8);
	ufbxt_assert(num_bottom == 8);

	ufbx_free_mesh(subdivided);
}
#endif

UFBXT_FILE_TEST(maya_subsurf_3x_cube)
#if UFBXT_IMPL
{
}
#endif

UFBXT_FILE_TEST(maya_subsurf_3x_cube_crease)
#if UFBXT_IMPL
{
}
#endif

#if UFBXT_IMPL
typedef struct {
	ufbx_vec3 position;
	ufbx_vec3 normal;
} ufbxt_vertex_pn;
#endif

UFBXT_FILE_TEST(blender_293_half_smooth_cube)
#if UFBXT_IMPL
{
	ufbx_node *node = ufbx_find_node(scene, "Cube");
	ufbxt_assert(node && node->mesh);
	ufbx_mesh *mesh = node->mesh;
	ufbxt_assert(mesh->num_indices == 6*4);
	ufbxt_assert(mesh->num_triangles == 6*2);

	ufbxt_vertex_pn vertices[64];
	uint32_t indices[64];
	size_t num_indices = 0;

	uint32_t tri[64];
	for (size_t fi = 0; fi < mesh->num_faces; fi++) {
		size_t num_tris = ufbx_triangulate_face(tri, 64, mesh, mesh->faces.data[fi]);
		for (size_t ti = 0; ti < num_tris * 3; ti++) {
			vertices[num_indices].position = ufbx_get_vertex_vec3(&mesh->vertex_position, tri[ti]);
			vertices[num_indices].normal = ufbx_get_vertex_vec3(&mesh->vertex_normal, tri[ti]);
			num_indices++;
		}
	}

	ufbx_vertex_stream stream = { vertices, num_indices, sizeof(ufbxt_vertex_pn) };
	size_t num_vertices = ufbx_generate_indices(&stream, 1, indices, num_indices, NULL, NULL);
	ufbxt_assert(num_vertices == 12);
}
#endif

UFBXT_FILE_TEST_ALT(generate_indices_multi_stream, blender_293_half_smooth_cube_7400_binary)
#if UFBXT_IMPL
{
	ufbx_node *node = ufbx_find_node(scene, "Cube");
	ufbxt_assert(node && node->mesh);
	ufbx_mesh *mesh = node->mesh;
	ufbxt_assert(mesh->num_indices == 6*4);
	ufbxt_assert(mesh->num_triangles == 6*2);

	ufbx_vec3 positions[64];
	ufbx_vec3 normals[64];
	uint32_t indices[64];
	size_t num_indices = 0;

	uint32_t tri[64];
	for (size_t fi = 0; fi < mesh->num_faces; fi++) {
		size_t num_tris = ufbx_triangulate_face(tri, 64, mesh, mesh->faces.data[fi]);
		for (size_t ti = 0; ti < num_tris * 3; ti++) {
			positions[num_indices] = ufbx_get_vertex_vec3(&mesh->vertex_position, tri[ti]);
			normals[num_indices] = ufbx_get_vertex_vec3(&mesh->vertex_normal, tri[ti]);
			num_indices++;
		}
	}

	ufbx_vertex_stream streams[] = {
		{ positions, num_indices, sizeof(ufbx_vec3) },
		{ normals, num_indices, sizeof(ufbx_vec3) },
	};
	size_t num_vertices = ufbx_generate_indices(streams, 2, indices, num_indices, NULL, NULL);
	ufbxt_assert(num_vertices == 12);
}
#endif

UFBXT_FILE_TEST(maya_vertex_crease_single)
#if UFBXT_IMPL
{
}
#endif

UFBXT_TEST(maya_vertex_crease_single_lefthanded)
#if UFBXT_IMPL
{
	ufbxt_diff_error err = { 0 };

	ufbxt_obj_file *obj_file = ufbxt_load_obj_file("maya_vertex_crease_single_lefthanded", NULL);

	char path[512];
	ufbxt_file_iterator iter = { "maya_vertex_crease_single" };
	while (ufbxt_next_file(&iter, path, sizeof(path))) {
		for (uint32_t i = 0; i < UFBX_MIRROR_AXIS_COUNT; i++) {
			ufbxt_hintf("mirror_axis=%u", i);

			ufbx_load_opts opts = { 0 };

			opts.target_axes = ufbx_axes_left_handed_y_up;
			opts.handedness_conversion_axis = (ufbx_mirror_axis)i;
			opts.reverse_winding = (i == 0);

			ufbx_error error;
			ufbx_scene *scene = ufbx_load_file(path, &opts, &error);
			if (!scene) ufbxt_log_error(&error);
			ufbxt_assert(scene);

			ufbxt_check_scene(scene);
			ufbxt_diff_to_obj(scene, obj_file, &err, 0);
			ufbx_free_scene(scene);
		}
	}

	ufbxt_logf(".. Absolute diff: avg %.3g, max %.3g (%zu tests)", err.sum / (ufbx_real)err.num, err.max, err.num);
	free(obj_file);
}
#endif

UFBXT_FILE_TEST(maya_vertex_crease)
#if UFBXT_IMPL
{
}
#endif

UFBXT_TEST(maya_vertex_crease_lefthanded)
#if UFBXT_IMPL
{
	ufbxt_diff_error err = { 0 };

	ufbxt_obj_file *obj_file = ufbxt_load_obj_file("maya_vertex_crease_lefthanded", NULL);

	char path[512];
	ufbxt_file_iterator iter = { "maya_vertex_crease" };
	while (ufbxt_next_file(&iter, path, sizeof(path))) {
		for (uint32_t i = 0; i < UFBX_MIRROR_AXIS_COUNT; i++) {
			ufbxt_hintf("mirror_axis=%u", i);

			ufbx_load_opts opts = { 0 };

			opts.target_axes = ufbx_axes_left_handed_y_up;
			opts.handedness_conversion_axis = (ufbx_mirror_axis)i;
			opts.reverse_winding = (i == 0);

			ufbx_error error;
			ufbx_scene *scene = ufbx_load_file(path, &opts, &error);
			if (!scene) ufbxt_log_error(&error);
			ufbxt_assert(scene);

			ufbxt_check_scene(scene);
			ufbxt_diff_to_obj(scene, obj_file, &err, 0);
			ufbx_free_scene(scene);
		}
	}

	ufbxt_logf(".. Absolute diff: avg %.3g, max %.3g (%zu tests)", err.sum / (ufbx_real)err.num, err.max, err.num);
	free(obj_file);
}
#endif

UFBXT_FILE_TEST(blender_312x_vertex_crease)
#if UFBXT_IMPL
{
}
#endif

UFBXT_TEST(generate_indices_empty_vertex)
#if UFBXT_IMPL
{
	uint32_t indices[9];
	ufbx_error error;
	size_t num_vertices = ufbx_generate_indices(NULL, 0, indices, 9, NULL, &error);
	ufbxt_assert(num_vertices == 0);
	ufbxt_assert(error.type == UFBX_ERROR_ZERO_VERTEX_SIZE);
}
#endif

UFBXT_TEST(generate_indices_no_indices)
#if UFBXT_IMPL
{
	ufbx_vec3 vertices[] = {
		{ 0.0f, 0.0f, 0.0f },
		{ 1.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f },
	};
	ufbx_vertex_stream streams[] = {
		{ vertices, 3, sizeof(ufbx_vec3) },
	};
	ufbx_error error;
	uint32_t indices[9];
	size_t num_vertices = ufbx_generate_indices(streams, 1, indices, 0, NULL, &error);
	ufbxt_assert(num_vertices == 0);
	ufbxt_assert(error.type == UFBX_ERROR_NONE);
}
#endif

UFBXT_TEST(generate_indices_truncated_stream)
#if UFBXT_IMPL
{
	ufbx_vec3 vertices[] = {
		{ 0.0f, 0.0f, 0.0f },
		{ 1.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f },
	};
	ufbx_vertex_stream streams[] = {
		{ vertices, 2, sizeof(ufbx_vec3) },
	};
	ufbx_error error;
	uint32_t indices[9];
	size_t num_vertices = ufbx_generate_indices(streams, 1, indices, 3, NULL, &error);
	ufbxt_assert(num_vertices == 0);
	ufbxt_assert(error.type == UFBX_ERROR_TRUNCATED_VERTEX_STREAM);
	ufbxt_assert(!strcmp(error.info, "0"));
	ufbxt_assert(error.info_length == 1);
}
#endif

