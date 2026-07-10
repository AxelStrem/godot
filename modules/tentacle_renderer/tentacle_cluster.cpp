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
#include "scene/resources/mesh.h"

// Per-LOD segment counts (must match the declaration in .h).
const int TentacleCluster::SEGMENTS_PER_LOD[4] = { 64, 32, 16, 0 };

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

void TentacleCluster::_rebuild_mesh() {
	if (!_mesh.is_valid()) {
		_mesh.instantiate();
		set_mesh(_mesh);
	}

	int nseg = _lod_segments();
	if (nseg < 2 || _tentacles.is_empty()) {
		_mesh->clear_surfaces();
		return;
	}

	// ---- Build vertex & index arrays ----
	// Layout per tentacle:  2*(nseg+1) vertices,  6*nseg triangle indices.
	// Vertex attributes: POSITION (world-space guide point),
	//                    NORMAL   (tentacle direction, normalized),
	//                    TANGENT  (progress, thickness, noise_seed, side_flag).
	const int verts_per_tentacle = 2 * (nseg + 1);
	const int idxs_per_tentacle = 6 * nseg;
	const int total_verts = _tentacles.size() * verts_per_tentacle;
	const int total_idxs = _tentacles.size() * idxs_per_tentacle;

	PackedVector3Array positions;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedInt32Array indices;

	positions.resize(total_verts);
	normals.resize(total_verts);
	tangents.resize(total_verts * 4); // 4 floats per vertex
	indices.resize(total_idxs);

	Vector3 *pos_w = positions.ptrw();
	Vector3 *nrm_w = normals.ptrw();
	float *tan_w = tangents.ptrw();
	int32_t *idx_w = indices.ptrw();

	int base_vert = 0;
	int base_idx = 0;

	for (int t = 0; t < _tentacles.size(); t++) {
		const TentacleEntry &entry = _tentacles[t];
		Vector3 start = entry.start;
		Vector3 end = entry.end;
		Vector3 dir = (end - start).normalized();

		float noise_seed = _hash_to_float(entry.id);

		for (int i = 0; i <= entry.segments; i++) {
			float progress = float(i) / float(MAX(entry.segments, 1));
			Vector3 pt = start.lerp(end, progress);

			// Left vertex (side = -1)
			int li = base_vert + 2 * i;
			pos_w[li] = pt;
			nrm_w[li] = dir;
			tan_w[li * 4 + 0] = progress;
			tan_w[li * 4 + 1] = entry.thickness;
			tan_w[li * 4 + 2] = noise_seed;
			tan_w[li * 4 + 3] = -1.0f;

			// Right vertex (side = +1)
			int ri = li + 1;
			pos_w[ri] = pt;
			nrm_w[ri] = dir;
			tan_w[ri * 4 + 0] = progress;
			tan_w[ri * 4 + 1] = entry.thickness;
			tan_w[ri * 4 + 2] = noise_seed;
			tan_w[ri * 4 + 3] = 1.0f;
		}

		// Triangle indices — two per segment.
		// Segment i:  vertices at 2i (L), 2i+1 (R), 2(i+1) (L), 2(i+1)+1 (R).
		// Triangles:  (L0, R0, L1) and (R0, R1, L1).
		for (int i = 0; i < entry.segments; i++) {
			int i0 = base_vert + 2 * i;       // L_i
			int i1 = i0 + 1;                   // R_i
			int i2 = i0 + 2;                   // L_{i+1}
			int i3 = i0 + 3;                   // R_{i+1}
			int bi = base_idx + 6 * i;
			idx_w[bi + 0] = i0;
			idx_w[bi + 1] = i1;
			idx_w[bi + 2] = i2;
			idx_w[bi + 3] = i1;
			idx_w[bi + 4] = i3;
			idx_w[bi + 5] = i2;
		}

		base_vert += verts_per_tentacle;
		base_idx += idxs_per_tentacle;
	}

	// ---- Upload to ArrayMesh ----
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = positions;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_INDEX] = indices;

	_mesh->clear_surfaces();
	_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), 0);
}

// ============================================================================
// Public API
// ============================================================================

void TentacleCluster::add_tentacle(int p_id, const Vector3 &p_start, const Vector3 &p_end, float p_thickness) {
	ERR_FAIL_COND_MSG(_id_to_index.has(p_id),
			vformat("TentacleCluster: tentacle %d already exists in this cluster.", p_id));

	TentacleEntry entry;
	entry.id = p_id;
	entry.start = p_start;
	entry.end = p_end;
	entry.thickness = p_thickness;
	entry.segments = _lod_segments();

	_id_to_index[p_id] = _tentacles.size();
	_tentacles.append(entry);

	_rebuild_mesh();
}

void TentacleCluster::remove_tentacle(int p_id) {
	ERR_FAIL_COND(!_id_to_index.has(p_id));

	int idx = _id_to_index[p_id];
	_id_to_index.erase(p_id);

	// Swap-remove for O(1) removal.
	int last_idx = _tentacles.size() - 1;
	if (idx != last_idx) {
		_tentacles.set(idx, _tentacles[last_idx]);
		_id_to_index[_tentacles[idx].id] = idx;
	}
	_tentacles.resize(last_idx);

	_rebuild_mesh();
}

bool TentacleCluster::has_tentacle(int p_id) const {
	return _id_to_index.has(p_id);
}

void TentacleCluster::clear() {
	_tentacles.clear();
	_id_to_index.clear();
	_rebuild_mesh();
}

void TentacleCluster::set_lod(LODLevel p_lod) {
	if (_current_lod == p_lod) {
		return;
	}
	_current_lod = p_lod;

	if (p_lod == LOD_CULLED) {
		hide();
	} else {
		show();
		// Update per-entry segment counts and rebuild.
		int new_seg = _lod_segments();
		bool changed = false;
		for (TentacleEntry &e : _tentacles) {
			if (e.segments != new_seg) {
				e.segments = new_seg;
				changed = true;
			}
		}
		if (changed) {
			_rebuild_mesh();
		}
	}
}

// ============================================================================
// Lifecycle
// ============================================================================

void TentacleCluster::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_tentacle", "id", "start", "end", "thickness"), &TentacleCluster::add_tentacle, DEFVAL(0.3f));
	ClassDB::bind_method(D_METHOD("remove_tentacle", "id"), &TentacleCluster::remove_tentacle);
	ClassDB::bind_method(D_METHOD("has_tentacle", "id"), &TentacleCluster::has_tentacle);
	ClassDB::bind_method(D_METHOD("clear"), &TentacleCluster::clear);
	ClassDB::bind_method(D_METHOD("get_tentacle_count"), &TentacleCluster::get_tentacle_count);

	ClassDB::bind_method(D_METHOD("set_lod", "lod"), &TentacleCluster::set_lod);
	ClassDB::bind_method(D_METHOD("get_lod"), &TentacleCluster::get_lod);

	BIND_ENUM_CONSTANT(LOD_HIGH);
	BIND_ENUM_CONSTANT(LOD_MEDIUM);
	BIND_ENUM_CONSTANT(LOD_LOW);
	BIND_ENUM_CONSTANT(LOD_CULLED);
}

TentacleCluster::TentacleCluster() {
	set_cast_shadows_setting(SHADOW_CASTING_SETTING_OFF);
}

TentacleCluster::~TentacleCluster() {
}
