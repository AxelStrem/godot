/**************************************************************************/
/*  spore_grid.h                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

// ---------------------------------------------------------------------------
// SporeGrid — multi-level spatial hash for spore proximity queries.
//
// Spores are partitioned across 4 levels based on their current radius:
//   Level 0: cell=1,  radius [0, 2)
//   Level 1: cell=4,  radius [2, 8)
//   Level 2: cell=16, radius [8, 32)
//   Level 3: cell=64, radius [32, ∞)
//
// This keeps the number of cells searched per query bounded regardless
// of spore size: cell_size grows with max_radius, so the cell-range
// ratio stays small (~3 cells per dimension at each level).
// ---------------------------------------------------------------------------

class SporeGrid : public Object {
	GDCLASS(SporeGrid, Object);

	static constexpr int LEVEL_COUNT = 4;

	struct Level {
		float cell_size;
		float min_radius; // inclusive
		float max_radius; // exclusive (except last level = INF)

		// grid_key → list of spore IDs whose centers fall in this cell
		HashMap<Vector3i, Vector<int32_t>> cells;
	};

	Level _levels[LEVEL_COUNT];

	// Per-spore bookkeeping so remove/migrate is O(1):
	//   _spore_level[id] = which level the spore lives in
	//   _spore_cell[id]  = the precise cell key (needed to erase from HashMap)
	HashMap<int32_t, int> _spore_level;
	HashMap<int32_t, Vector3i> _spore_cell;

protected:
	static void _bind_methods();

public:
	SporeGrid() {}

	// Grid resolution helpers (used by SporeManager).
	static int _level_for_radius(float p_radius);
	static Vector3i _cell_key(const Vector3 &p_pos, float p_cell_size);

	// Place a spore into the grid (or move it if already present).
	void insert(int32_t p_id, const Vector3 &p_pos, float p_radius);
	void remove(int32_t p_id);
	void migrate(int32_t p_id, const Vector3 &p_old_pos, float p_old_radius, const Vector3 &p_new_pos, float p_new_radius);

	// Collect all spore IDs whose surface may intersect a sphere.
	void query_sphere(const Vector3 &p_center, float p_query_radius, Vector<int32_t> &r_out) const;

	// Collect all spore IDs near a point (tight query for parent-finding).
	void query_nearby(const Vector3 &p_center, Vector<int32_t> &r_out) const;

	// For debugging.
	int get_spore_count() const;
};

// Inline helpers shared with SporeManager.
inline int SporeGrid::_level_for_radius(float p_radius) {
	if (p_radius < 2.0f) {
		return 0;
	}
	if (p_radius < 8.0f) {
		return 1;
	}
	if (p_radius < 32.0f) {
		return 2;
	}
	return 3;
}

inline Vector3i SporeGrid::_cell_key(const Vector3 &p_pos, float p_cell_size) {
	return Vector3i(
		int(Math::floor(p_pos.x / p_cell_size)),
		int(Math::floor(p_pos.y / p_cell_size)),
		int(Math::floor(p_pos.z / p_cell_size)));
}
