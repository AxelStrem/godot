/**************************************************************************/
/*  tentacle_cluster.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "tentacle_cluster.h"

#include "core/math/vector3.h"
#include "core/object/class_db.h"
#include "core/templates/hashfuncs.h"
#include "scene/resources/multimesh.h"

// Per-LOD segment counts (must match the declaration in .h).
// Only CULLED gets 0 segments; the other four are active.
const int TentacleCluster::SEGMENTS_PER_LOD[5] = { 96, 48, 16, 8, 0 };

// ============================================================================
// Internal helpers
// ============================================================================

int TentacleCluster::_lod_segments() const {
	return SEGMENTS_PER_LOD[_current_lod];
}

float TentacleCluster::_hash_to_float(int p_id) {
	// Simple hash so each tentacle gets a unique noise-seed value
	// while staying within a reasonable [-10, 10] range.
	uint32_t h = hash_djb2_one_32(uint32_t(p_id));
	return float(h % 20000u) * 0.001f - 10.0f;
}

// Build (or rebuild) the shared ArrayMesh for the current LOD.
// This is a straight Z-aligned strip from (0,0,0) to (0,0,1) with
// both left/right vertices at each centerline point.
// The mesh is instanced by MultiMesh — the instance transform maps
// (0,0,progress) → start + dir · progress · length in world space.
void TentacleCluster::_create_shared_mesh() {
	int nseg = _lod_segments();
	if (nseg < 2) {
		if (_shared_mesh.is_valid()) {
			_shared_mesh->clear_surfaces();
		}
		return;
	}

	if (!_shared_mesh.is_valid()) {
		_shared_mesh.instantiate();
	} else if (_shared_mesh->get_surface_count() > 0) {
		_shared_mesh->clear_surfaces();
	}

	const int vert_count = 2 * (nseg + 1);
	const int idx_count = 6 * nseg;

	PackedVector3Array positions;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedInt32Array indices;

	positions.resize(vert_count);
	normals.resize(vert_count);
	uvs.resize(vert_count);
	indices.resize(idx_count);

	Vector3 *pos_w = positions.ptrw();
	Vector3 *nrm_w = normals.ptrw();
	Vector2 *uv_w = uvs.ptrw();
	int32_t *idx_w = indices.ptrw();

	// Build a Z-aligned strip: each segment has a left (-1) and right (+1) vertex.
	// NORMAL = forward (0,0,1) so the shader can use it as the billboard tangent.
	for (int i = 0; i <= nseg; i++) {
		float progress = float(i) / float(nseg);
		int li = 2 * i;
		int ri = 2 * i + 1;

		pos_w[li] = Vector3(0, 0, progress);
		pos_w[ri] = Vector3(0, 0, progress);
		nrm_w[li] = Vector3(0, 0, 1);
		nrm_w[ri] = Vector3(0, 0, 1);
		uv_w[li] = Vector2(-1, 0); // left  side-flag (unused, kept for format)
		uv_w[ri] = Vector2(1, 0);  // right side-flag (unused, kept for format)
	}

	for (int i = 0; i < nseg; i++) {
		int i0 = 2 * i;
		int i1 = i0 + 1;
		int i2 = i0 + 2;
		int i3 = i0 + 3;
		int bi = 6 * i;
		idx_w[bi + 0] = i0;
		idx_w[bi + 1] = i1;
		idx_w[bi + 2] = i2;
		idx_w[bi + 3] = i1;
		idx_w[bi + 4] = i3;
		idx_w[bi + 5] = i2;
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = positions;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;

	_shared_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), 0);

	// Bind to the MultiMesh (preserves existing instance data).
	if (_multimesh.is_valid()) {
		_multimesh->set_mesh(_shared_mesh);
	}
}

// Upload all cached transforms and custom data to the MultiMesh.
// Called after every add/remove because set_instance_count() reallocates
// the GPU buffer, destroying all previously-uploaded data.
void TentacleCluster::_sync_multimesh() {
	int count = _cached_transforms.size();
	_multimesh->set_instance_count(count);
	for (int i = 0; i < count; i++) {
		_multimesh->set_instance_transform(i, _cached_transforms[i]);
		_multimesh->set_instance_custom_data(i, _cached_custom_data[i]);
	}
}

// ============================================================================
// Public API
// ============================================================================

void TentacleCluster::add_tentacle(int p_id, const Vector3 &p_start, const Vector3 &p_end, float p_born_at, float p_thickness) {
	add_tentacle_no_sync(p_id, p_start, p_end, p_born_at, p_thickness);
	sync();
}

void TentacleCluster::add_tentacle_no_sync(int p_id, const Vector3 &p_start, const Vector3 &p_end, float p_born_at, float p_thickness) {
	ERR_FAIL_COND_MSG(_id_to_index.has(p_id),
			vformat("TentacleCluster: tentacle %d already exists in this cluster.", p_id));

	Vector3 dir = (p_end - p_start).normalized();
	float len = p_start.distance_to(p_end);

	// Build an orthonormal basis where Z = tentacle direction.
	Vector3 up = Vector3(0, 1, 0);
	if (Math::abs(dir.dot(up)) > 0.999f) {
		up = Vector3(1, 0, 0);
	}
	Vector3 right = dir.cross(up).normalized();
	up = right.cross(dir).normalized();

	Transform3D xform;
	xform.origin = p_start;
	// Build orthonormal basis with length baked into the Z column (dir * len).
	// NOTE: Do NOT use Basis::scaled() — it left-multiplies (S * M), scaling
	// global axes. We need the local Z axis (column 2) scaled, which is simply
	// Basis(right, up, dir * len).
	xform.basis = Basis(right, up, dir * len);

	Color custom = Color(p_born_at, _hash_to_float(p_id), p_thickness, 0);

	int idx = _cached_transforms.size();
	_cached_transforms.push_back(xform);
	_cached_custom_data.push_back(custom);
	_index_to_id.push_back(p_id);
	_id_to_index[p_id] = idx;
}

void TentacleCluster::remove_tentacle(int p_id) {
	remove_tentacle_no_sync(p_id);
	sync();
}

void TentacleCluster::remove_tentacle_no_sync(int p_id) {
	ERR_FAIL_COND(!_id_to_index.has(p_id));

	int idx = _id_to_index[p_id];
	int last = _cached_transforms.size() - 1;

	// Swap-remove in our local caches (MultiMesh data is re-uploaded below).
	if (idx != last) {
		_cached_transforms.set(idx, _cached_transforms[last]);
		_cached_custom_data.set(idx, _cached_custom_data[last]);

		int moved_id = _index_to_id[last];
		_index_to_id.set(idx, moved_id);
		_id_to_index[moved_id] = idx;
	}

	_cached_transforms.resize(last);
	_cached_custom_data.resize(last);
	_index_to_id.resize(last);
	_id_to_index.erase(p_id);
}

void TentacleCluster::sync() {
	_sync_multimesh();
}

bool TentacleCluster::has_tentacle(int p_id) const {
	return _id_to_index.has(p_id);
}

void TentacleCluster::clear() {
	_cached_transforms.clear();
	_cached_custom_data.clear();
	_index_to_id.clear();
	_id_to_index.clear();
	_multimesh->set_instance_count(0);
}

void TentacleCluster::set_lod(LODLevel p_lod) {
	if (_current_lod == p_lod) {
		return;
	}
	_current_lod = p_lod;

	if (p_lod == LOD_CULLED) {
		hide();
	} else {
		_create_shared_mesh(); // rebuild shared mesh with new segment count
		show();
	}
}

// ============================================================================
// Lifecycle
// ============================================================================

void TentacleCluster::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_tentacle", "id", "start", "end", "born_at", "thickness"), &TentacleCluster::add_tentacle, DEFVAL(0.3f));
	ClassDB::bind_method(D_METHOD("add_tentacle_no_sync", "id", "start", "end", "born_at", "thickness"), &TentacleCluster::add_tentacle_no_sync, DEFVAL(0.3f));
	ClassDB::bind_method(D_METHOD("remove_tentacle", "id"), &TentacleCluster::remove_tentacle);
	ClassDB::bind_method(D_METHOD("remove_tentacle_no_sync", "id"), &TentacleCluster::remove_tentacle_no_sync);
	ClassDB::bind_method(D_METHOD("sync"), &TentacleCluster::sync);
	ClassDB::bind_method(D_METHOD("has_tentacle", "id"), &TentacleCluster::has_tentacle);
	ClassDB::bind_method(D_METHOD("clear"), &TentacleCluster::clear);
	ClassDB::bind_method(D_METHOD("get_tentacle_count"), &TentacleCluster::get_tentacle_count);
	ClassDB::bind_method(D_METHOD("set_origin", "origin"), &TentacleCluster::set_origin);

	ClassDB::bind_method(D_METHOD("set_lod", "lod"), &TentacleCluster::set_lod);
	ClassDB::bind_method(D_METHOD("get_lod"), &TentacleCluster::get_lod);

	BIND_ENUM_CONSTANT(LOD_HIGH);
	BIND_ENUM_CONSTANT(LOD_MEDIUM);
	BIND_ENUM_CONSTANT(LOD_LOW);
	BIND_ENUM_CONSTANT(LOD_VERY_LOW);
	BIND_ENUM_CONSTANT(LOD_CULLED);
}

TentacleCluster::TentacleCluster() {
	set_cast_shadows_setting(SHADOW_CASTING_SETTING_OFF);

	_multimesh.instantiate();
	_multimesh->set_transform_format(MultiMesh::TRANSFORM_3D);
	_multimesh->set_use_custom_data(true);
	set_multimesh(_multimesh);

	_create_shared_mesh();
}

TentacleCluster::~TentacleCluster() {
}
