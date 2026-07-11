/**************************************************************************/
/*  tentacle_cluster.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/*  A MultiMeshInstance3D that batches GPU-simulated tentacles.           */
/*  One shared ArrayMesh (straight strip) is instanced per tentacle.      */
/*  Noise wiggle, billboarding, and taper happen in the vertex shader.    */
/*                                                                        */
/*  One cluster covers a ~20×20 m world region.  Add/remove is O(1)      */
/*  GPU buffer writes (swap-remove).  LOD changes swap the shared mesh.   */
/**************************************************************************/

#pragma once

#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/multimesh.h"

#include "core/math/vector3.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

class ArrayMesh;

class TentacleCluster : public MultiMeshInstance3D {
	GDCLASS(TentacleCluster, MultiMeshInstance3D);

public:
	// ---- LOD levels ----
	enum LODLevel {
		LOD_HIGH = 0,   // 64 segments, full noise   (near)
		LOD_MEDIUM = 1, // 32 segments, half noise
		LOD_LOW = 2,    // 16 segments, no noise (straight lines)
		LOD_CULLED = 3  // hidden entirely (distance)
	};

private:
	Ref<ArrayMesh> _shared_mesh;
	Ref<MultiMesh> _multimesh;

	// Per-instance tracking: tentacle id ↔ MultiMesh instance index.
	HashMap<int, int> _id_to_index;
	Vector<int> _index_to_id;   // parallel to MultiMesh instances

	// Local caches — MultiMesh::set_instance_count() reallocates the GPU
	// buffer (zeroing everything), so we must keep our own copies and
	// re-upload after every resize.
	Vector<Transform3D> _cached_transforms;
	Vector<Color> _cached_custom_data;

	LODLevel _current_lod = LOD_HIGH;
	Vector3 _origin; // grid cell center (for LOD distance checks only)

	// Per-LOD segment counts.
	static const int SEGMENTS_PER_LOD[4];

	// ---- Helpers ----
	void _create_shared_mesh();
	void _sync_multimesh();
	int _lod_segments() const;
	static float _hash_to_float(int p_id);

protected:
	static void _bind_methods();

public:
	// ---- Tentacle management ----
	void add_tentacle(int p_id, const Vector3 &p_start, const Vector3 &p_end, float p_born_at, float p_thickness = 0.3f);
	void remove_tentacle(int p_id);
	bool has_tentacle(int p_id) const;
	void clear();
	int get_tentacle_count() const { return _index_to_id.size(); }

	// ---- LOD ----
	void set_lod(LODLevel p_lod);
	LODLevel get_lod() const { return _current_lod; }

	// ---- Origin (grid cell center for LOD checks) ----
	void set_origin(const Vector3 &p_origin) { _origin = p_origin; }
	Vector3 get_origin() const { return _origin; }

	TentacleCluster();
	~TentacleCluster();
};

VARIANT_ENUM_CAST(TentacleCluster::LODLevel);
