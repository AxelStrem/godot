/**************************************************************************/
/*  tentacle_cluster.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/*  A MeshInstance3D that holds a batch of GPU-simulated tentacles.       */
/*  The CPU generates straight-line guide points only; noise wiggle,      */
/*  billboarding, and taper happen in the spatial vertex shader.          */
/*                                                                        */
/*  One cluster covers a ~20×20 m world region.  Tentacles are static     */
/*  after spawn so the mesh is only rebuilt on add/remove/LOD change.     */
/**************************************************************************/

#pragma once

#include "scene/3d/mesh_instance_3d.h"

#include "core/math/vector3.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

class Material;
class ArrayMesh;

class TentacleCluster : public MeshInstance3D {
	GDCLASS(TentacleCluster, MeshInstance3D);

public:
	// ---- LOD levels ----
	enum LODLevel {
		LOD_HIGH = 0,   // 64 segments, full noise   (near)
		LOD_MEDIUM = 1, // 32 segments, half noise
		LOD_LOW = 2,    // 16 segments, no noise (straight lines)
		LOD_CULLED = 3 // hidden entirely (distance)
	};

private:
	struct TentacleEntry {
		int id = 0;
		Vector3 start;
		Vector3 end;
		float thickness = 0.3f;
		int segments = 64; // current LOD segment count for this entry
	};

	// ---- Data ----
	Vector<TentacleEntry> _tentacles;
	HashMap<int, int> _id_to_index; // tentacle id → index in _tentacles

	Ref<ArrayMesh> _mesh;
	LODLevel _current_lod = LOD_HIGH;

	// Per-LOD segment counts.
	static const int SEGMENTS_PER_LOD[4];

	// ---- Helpers ----
	void _rebuild_mesh();
	int _lod_segments() const;
	static float _hash_to_float(int p_id);

protected:
	static void _bind_methods();

public:
	// ---- Tentacle management ----
	void add_tentacle(int p_id, const Vector3 &p_start, const Vector3 &p_end, float p_thickness = 0.3f);
	void remove_tentacle(int p_id);
	bool has_tentacle(int p_id) const;
	void clear();
	int get_tentacle_count() const { return _tentacles.size(); }

	// ---- LOD ----
	void set_lod(LODLevel p_lod);
	LODLevel get_lod() const { return _current_lod; }

	TentacleCluster();
	~TentacleCluster();
};

VARIANT_ENUM_CAST(TentacleCluster::LODLevel);
