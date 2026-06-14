/**************************************************************************/
/*  spore_grid.cpp                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "spore_grid.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/variant/array.h"
#include "core/variant/typed_array.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static const float LEVEL_CELL_SIZE[4] = { 1.0f, 4.0f, 16.0f, 64.0f };
static const float LEVEL_MAX_RADIUS[4] = { 2.0f, 8.0f, 32.0f, INFINITY };

// Pre-computed neighbour offsets for a 3×3×3 cube (27 entries).
// We'll generate the ranges dynamically because they vary per query.

// ---------------------------------------------------------------------------
// Bind methods (exposed to GDScript for debugging)
// ---------------------------------------------------------------------------

void SporeGrid::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_spore_count"), &SporeGrid::get_spore_count);
}

// ---------------------------------------------------------------------------
// Insert / Remove / Migrate
// ---------------------------------------------------------------------------

void SporeGrid::insert(int32_t p_id, const Vector3 &p_pos, float p_radius) {
	int level = _level_for_radius(p_radius);
	Vector3i key = _cell_key(p_pos, LEVEL_CELL_SIZE[level]);

	// Remove any previous entry (safe to call even if not present).
	if (_spore_level.has(p_id)) {
		remove(p_id);
	}

	_levels[level].cells[key].push_back(p_id);
	_spore_level[p_id] = level;
	_spore_cell[p_id] = key;
}

void SporeGrid::remove(int32_t p_id) {
	if (!_spore_level.has(p_id)) {
		return;
	}

	int level = _spore_level[p_id];
	Vector3i key = _spore_cell[p_id];

	HashMap<Vector3i, Vector<int32_t>> &cells = _levels[level].cells;
	if (cells.has(key)) {
		Vector<int32_t> &vec = cells[key];
		int32_t idx = vec.find(p_id);
		if (idx != -1) {
			vec.remove_at(idx);
		}
		if (vec.is_empty()) {
			cells.erase(key);
		}
	}

	_spore_level.erase(p_id);
	_spore_cell.erase(p_id);
}

void SporeGrid::migrate(int32_t p_id, const Vector3 &p_old_pos, float p_old_radius, const Vector3 &p_new_pos, float p_new_radius) {
	int old_level = _level_for_radius(p_old_radius);
	int new_level = _level_for_radius(p_new_radius);
	Vector3i new_key = _cell_key(p_new_pos, LEVEL_CELL_SIZE[new_level]);

	if (!_spore_level.has(p_id)) {
		// Not in grid yet — just insert.
		_levels[new_level].cells[new_key].push_back(p_id);
		_spore_level[p_id] = new_level;
		_spore_cell[p_id] = new_key;
		return;
	}

	int cur_level = _spore_level[p_id];
	Vector3i cur_key = _spore_cell[p_id];

	if (cur_level == new_level && cur_key == new_key) {
		return; // No change needed.
	}

	// Remove from old cell.
	HashMap<Vector3i, Vector<int32_t>> &cells = _levels[cur_level].cells;
	if (cells.has(cur_key)) {
		Vector<int32_t> &vec = cells[cur_key];
		int32_t idx = vec.find(p_id);
		if (idx != -1) {
			vec.remove_at(idx);
		}
		if (vec.is_empty()) {
			cells.erase(cur_key);
		}
	}

	// Insert into new cell.
	_levels[new_level].cells[new_key].push_back(p_id);
	_spore_level[p_id] = new_level;
	_spore_cell[p_id] = new_key;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

void SporeGrid::query_sphere(const Vector3 &p_center, float p_query_radius, Vector<int32_t> &r_out) const {
	for (int level = 0; level < LEVEL_COUNT; level++) {
		float cell_size = LEVEL_CELL_SIZE[level];
		float max_r = LEVEL_MAX_RADIUS[level];
		float search_range = max_r + p_query_radius;
		int cell_range = int(Math::ceil(search_range / cell_size));

		Vector3i center_cell = _cell_key(p_center, cell_size);

		for (int dx = -cell_range; dx <= cell_range; dx++) {
			for (int dy = -cell_range; dy <= cell_range; dy++) {
				for (int dz = -cell_range; dz <= cell_range; dz++) {
					Vector3i key = center_cell + Vector3i(dx, dy, dz);
					const HashMap<Vector3i, Vector<int32_t>> &cells = _levels[level].cells;
					const Vector<int32_t> *vec = cells.getptr(key);
					if (vec == nullptr) {
						continue;
					}
					for (int32_t id : *vec) {
						r_out.push_back(id);
					}
				}
			}
		}
	}
}

void SporeGrid::query_nearby(const Vector3 &p_center, Vector<int32_t> &r_out) const {
	// Tight query: only check level 0 and level 1 with small search range.
	// Used for parent-finding at spore spawn time.
	for (int level = 0; level <= 1; level++) {
		float cell_size = LEVEL_CELL_SIZE[level];
		// Search ±1 cell around center for nearby spores.
		Vector3i center_cell = _cell_key(p_center, cell_size);

		for (int dx = -1; dx <= 1; dx++) {
			for (int dy = -1; dy <= 1; dy++) {
				for (int dz = -1; dz <= 1; dz++) {
					Vector3i key = center_cell + Vector3i(dx, dy, dz);
					const HashMap<Vector3i, Vector<int32_t>> &cells = _levels[level].cells;
					const Vector<int32_t> *vec = cells.getptr(key);
					if (vec == nullptr) {
						continue;
					}
					for (int32_t id : *vec) {
						r_out.push_back(id);
					}
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

int SporeGrid::get_spore_count() const {
	return _spore_level.size();
}
