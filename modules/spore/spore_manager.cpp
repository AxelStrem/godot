/**************************************************************************/
/*  spore_manager.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "spore_manager.h"

#include "core/math/math_funcs.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"
#include "core/object/class_db.h"

// ---------------------------------------------------------------------------
// Growth parameters (mirrors GDScript constants)
// ---------------------------------------------------------------------------

static constexpr float MIN_RADIUS = 0.25f;
static constexpr float MAX_RADIUS_NORMAL = 20.0f;
static constexpr float MAX_RADIUS_STRAIN = 2.0f;

// Minimum radius when a spore is force-limited by a ward.
// Mirrors SporeConfig.WARD_MIN_SPORE_RADIUS in GDScript.
static constexpr float MIN_FORCE_LIMITED_RADIUS = 0.1f;

// Phase timing (seconds since start of growth, after start_delay).
static constexpr float PHASE1_DURATION = 0.6f;   // slower burst 0→0.5 (visible)
static constexpr float PHASE2_DURATION = 25.0f;  // slow pulsing growth (0.5→2.0)
static constexpr float PHASE3_DURATION = 60.0f;  // accelerating growth (2.0→cap)

static constexpr float PULSE_FREQ = 3.0f;         // pulse oscillations per second
static constexpr float PHASE2_PULSE_AMP = 0.1f;   // pulse amplitude during phase 2
static constexpr float PHASE3_PULSE_AMP_INITIAL = 0.15f;  // pulse amp at start of phase 3
static constexpr float PHASE4_PULSE_AMP = 0.05f;  // subtle pulse once mature

// Lifetime (seconds). When elapsed > lifetime, the spore is removed.
static constexpr float STRAIN_LIFETIME = 120.0f;
static constexpr float NORMAL_LIFETIME = 30.0f;
static constexpr float SHORT_LIFETIME = 15.0f;

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

SporeManager::SporeManager() {}
SporeManager::~SporeManager() {}

// ---------------------------------------------------------------------------
// ID allocation with free-list recycling
// ---------------------------------------------------------------------------

int SporeManager::_allocate_id() {
	if (_free_list.is_empty()) {
		int32_t id = _positions.size();
		_positions.push_back(Vector3());
		_spawn_times.push_back(0.0f);
		_radii.push_back(0.0001f);
		_force_limits.push_back(0.0f);
		_seed_offsets.push_back(0.0f);
		_states.push_back(STATE_DEAD);
		_profiles.push_back(PROFILE_NORMAL);
		_chamber_ids.push_back(-1);
		_alive.push_back(false);
		return id;
	}
	int32_t id = _free_list[_free_list.size() - 1];
	_free_list.resize(_free_list.size() - 1);
	return id;
}

void SporeManager::_free_id(int32_t p_id) {
	_alive.set(p_id, false);
	_states.set(p_id, STATE_DEAD);
	_free_list.push_back(p_id);
}

// ---------------------------------------------------------------------------
// Radius computation (no Tweens, no callbacks — pure function of time)
// ---------------------------------------------------------------------------

float SporeManager::_compute_radius(float p_elapsed, int p_profile, float p_seed_offset) const {
	// Phase 0: invisible delay while the parent tentacle connects.
	if (p_elapsed < _start_delay) {
		return 0.0f;
	}
	float t_elapsed = p_elapsed - _start_delay;

	float cap = (p_profile == PROFILE_STRAIN) ? MAX_RADIUS_STRAIN : MAX_RADIUS_NORMAL;
	float base;
	float pulse_amp;

	if (t_elapsed < PHASE1_DURATION) {
		// Phase 1: fast burst into view.
		float t = t_elapsed / PHASE1_DURATION;
		base = t * 0.5f;
		pulse_amp = 0.0f;
	} else if (t_elapsed < PHASE1_DURATION + PHASE2_DURATION) {
		// Phase 2: slow pulsing growth 0.5 → 2.0.
		float t = (t_elapsed - PHASE1_DURATION) / PHASE2_DURATION;
		base = 0.5f + t * 1.5f;
		pulse_amp = PHASE2_PULSE_AMP;
	} else if (t_elapsed < PHASE1_DURATION + PHASE2_DURATION + PHASE3_DURATION) {
		// Phase 3: accelerating growth 2.0 → cap.
		float phase3_elapsed = t_elapsed - PHASE1_DURATION - PHASE2_DURATION;
		float t = phase3_elapsed / PHASE3_DURATION;
		float t5 = t * t * t * t * t; // ease-in quintic (very slow start, ramps late)
		base = 2.0f + t5 * (cap - 2.0f);
		pulse_amp = PHASE3_PULSE_AMP_INITIAL * (1.0f - t); // pulse fades out
	} else {
		// Phase 4: stable at cap with subtle continuous pulse.
		base = cap;
		pulse_amp = PHASE4_PULSE_AMP;
	}

	float pulse = Math::sin(p_elapsed * PULSE_FREQ + p_seed_offset) * pulse_amp;
	return MAX(base + pulse, 0.0f);
}

// ---------------------------------------------------------------------------
// Public API — Spore lifecycle
// ---------------------------------------------------------------------------

int32_t SporeManager::add_spore(const Vector3 &p_pos, int p_profile, int p_chamber_id) {
	int32_t id = _allocate_id();
	_positions.set(id, p_pos);
	_spawn_times.set(id, -1.0f); // Will be set by the first update() call.
	_radii.set(id, 0.0001f);     // Reset stale radius from recycled ID.
	_force_limits.set(id, 0.0f); // Reset stale force-limit from recycled ID.
	_seed_offsets.set(id, Math::randf() * 6.2831853f);
	_states.set(id, STATE_START_DELAY);
	_profiles.set(id, (p_profile == PROFILE_STRAIN) ? PROFILE_STRAIN : PROFILE_NORMAL);
	_chamber_ids.set(id, p_chamber_id);
	_alive.set(id, true);

	// Insert into spatial grid with initial tiny radius.
	_grid.insert(id, p_pos, 0.0001f);

	// Push to alive_ids immediately so subsequent queries in the same
	// frame can find this spore (e.g. tentacle parent-finding for spores
	// spawned in rapid succession via staggered timers).
	_alive_ids.push_back(id);

	return id;
}

void SporeManager::remove_spore(int32_t p_id) {
	if (!_alive[p_id]) {
		return;
	}
	_grid.remove(p_id);
	_free_id(p_id);
}

void SporeManager::remove_spores_in_chamber(int p_chamber_id) {
	// Collect matching IDs first; removing while iterating would invalidate.
	Vector<int32_t> to_remove;
	for (int32_t id : _alive_ids) {
		if (_chamber_ids[id] == p_chamber_id) {
			to_remove.push_back(id);
		}
	}
	for (int32_t id : to_remove) {
		remove_spore(id);
	}
}

void SporeManager::set_spore_state(int32_t p_id, int p_state) {
	if (!_alive[p_id]) {
		return;
	}
	_states.set(p_id, (uint8_t)p_state);
}

int SporeManager::get_spore_state(int32_t p_id) const {
	if (!_alive[p_id]) {
		return STATE_DEAD;
	}
	return _states[p_id];
}

void SporeManager::set_spore_profile(int32_t p_id, int p_profile) {
	if (!_alive[p_id]) {
		return;
	}
	_profiles.set(p_id, (uint8_t)p_profile);
}

int SporeManager::get_spore_profile(int32_t p_id) const {
	if (!_alive[p_id]) {
		return PROFILE_NORMAL;
	}
	return _profiles[p_id];
}

void SporeManager::set_start_delay(float p_delay) {
	_start_delay = p_delay;
}

float SporeManager::get_start_delay() const {
	return _start_delay;
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------

void SporeManager::update(double p_delta, double p_total_time) {
	_last_total_time = p_total_time;

	for (int32_t id = 0; id < _alive.size(); id++) {
		if (!_alive[id]) {
			continue;
		}

		// Set spawn time on first update.
		if (_spawn_times[id] < 0.0f) {
			_spawn_times.set(id, p_total_time);
		}

		float elapsed = float(p_total_time - _spawn_times[id]);

		// Lifecycle death: remove spores that have exceeded their lifetime.
		float lifetime;
		switch (_profiles[id]) {
			case PROFILE_STRAIN:
				lifetime = STRAIN_LIFETIME;
				break;
			default:
				lifetime = NORMAL_LIFETIME;
				break;
		}
		if (elapsed > lifetime) {
			_grid.remove(id);
			_free_id(id);
			continue;
		}

		float old_radius = _radii[id];
		float new_radius = _compute_radius(elapsed, _profiles[id], _seed_offsets[id]);

		// Ward force-limit: smooth shrink toward the cap when a ward
		// restricts this spore, and smooth recovery when the spore
		// leaves the ward so it doesn't snap back to full radius.
		float limit = _force_limits[id];
		if (limit > 0.0f) {
			float target = MIN(new_radius, limit);
			target = MAX(target, MIN_FORCE_LIMITED_RADIUS);
			new_radius = Math::move_toward(old_radius, target, _force_limit_shrink_speed * float(p_delta));
		} else if (new_radius > old_radius + 0.001f) {
			// Smooth recovery: grow back to natural radius gradually.
			new_radius = Math::move_toward(old_radius, new_radius, _force_limit_shrink_speed * float(p_delta));
		}

		_radii.set(id, new_radius);

		// Update grid if the level changed.
		int old_level = SporeGrid::_level_for_radius(old_radius);
		int new_level = SporeGrid::_level_for_radius(new_radius);
		if (old_level != new_level) {
			_grid.migrate(id, _positions[id], old_radius, _positions[id], new_radius);
		}
	}

	// Rebuild compact alive list for fast linear-scan queries.
	_alive_ids.clear();
	_alive_ids.reserve(_alive.size() - _free_list.size());
	for (int32_t id = 0; id < _alive.size(); id++) {
		if (_alive[id]) {
			_alive_ids.push_back(id);
		}
	}
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------

float SporeManager::get_spore_radius(int32_t p_id) const {
	if (!_alive[p_id]) {
		return 0.0f;
	}
	return _radii[p_id];
}

Vector3 SporeManager::get_spore_position(int32_t p_id) const {
	if (!_alive[p_id]) {
		return Vector3();
	}
	return _positions[p_id];
}

bool SporeManager::is_spore_alive(int32_t p_id) const {
	return _alive[p_id];
}

int SporeManager::get_spore_count() const {
	return _alive.size() - _free_list.size();
}

int SporeManager::get_active_spore_count() const {
	int count = 0;
	for (int i = 0; i < _alive.size(); i++) {
		if (_alive[i] && _states[i] == STATE_ACTIVE) {
			count++;
		}
	}
	return count;
}

// ---------------------------------------------------------------------------
// Spatial queries — linear scan over compact _alive_ids array.
// For typical spore counts (≤ 500) this is faster than any spatial index
// due to cache locality, branch predictability, and zero hashing overhead.
// If spore counts grow beyond ~5000, layer on coarse bucketing via SporeGrid
// to skip entire buckets of spores in a single branch.
// ---------------------------------------------------------------------------

Vector<int32_t> SporeManager::query_nearby(const Vector3 &p_pos) const {
	// Cell-range query (levels 0-1, ±1 cell). Equivalent to the old
	// SporeGrid::query_nearby but much cheaper.
	Vector<int32_t> result;
	for (int32_t id : _alive_ids) {
		// Nearby = within 8 units (level 0: cell=1, level 1: cell=4, ±1 cell each).
		float dist2 = p_pos.distance_squared_to(_positions[id]);
		if (dist2 < 64.0f) { // 8²
			result.push_back(id);
		}
	}
	return result;
}

Vector<int32_t> SporeManager::query_sphere(const Vector3 &p_center, float p_radius) const {
	Vector<int32_t> result;
	float r2 = p_radius * p_radius;
	for (int32_t id : _alive_ids) {
		float total_r = p_radius + _radii[id];
		if (_positions[id].distance_squared_to(p_center) < total_r * total_r) {
			result.push_back(id);
		}
	}
	return result;
}

Vector<int32_t> SporeManager::query_spores_in_range(const Vector3 &p_pos, float p_radius) const {
	// Distance-filtered query: surface of query sphere intersects spore surface.
	Vector<int32_t> result;
	float r2 = p_radius * p_radius;
	for (int32_t id : _alive_ids) {
		float total_r = p_radius + _radii[id];
		if (_positions[id].distance_squared_to(p_pos) < total_r * total_r) {
			result.push_back(id);
		}
	}
	return result;
}

bool SporeManager::is_any_spore_in_range(const Vector3 &p_center, float p_radius) const {
	// Early-out variant for damage/destructible checks.
	float r2 = p_radius * p_radius;
	for (int32_t id : _alive_ids) {
		float total_r = p_radius + _radii[id];
		if (_positions[id].distance_squared_to(p_center) < total_r * total_r) {
			return true;
		}
	}
	return false;
}

bool SporeManager::is_any_dangerous_spore_in_range(const Vector3 &p_center, float p_radius) const {
	// Like is_any_spore_in_range, but skips spores suppressed by a ward
	// (force_limits[id] > 0).  Used for player damage checks so that
	// ward-suppressed spores don't kill players.
	float r2 = p_radius * p_radius;
	for (int32_t id : _alive_ids) {
		if (_force_limits[id] > 0.0f) {
			continue;
		}
		float total_r = p_radius + _radii[id];
		if (_positions[id].distance_squared_to(p_center) < total_r * total_r) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Ward management
// ---------------------------------------------------------------------------

void SporeManager::_rebuild_ward_grid() {
	_ward_grid.clear();
	for (int32_t i = 0; i < _wards.size(); i++) {
		Vector3i key(
			int(Math::floor(_wards[i].pos.x / WARD_CELL_SIZE)),
			int(Math::floor(_wards[i].pos.y / WARD_CELL_SIZE)),
			int(Math::floor(_wards[i].pos.z / WARD_CELL_SIZE))
		);
		_ward_grid[key].push_back(i);
	}
}

void SporeManager::set_wards(const TypedArray<Vector3> &p_positions, const TypedArray<float> &p_radii) {
	_wards.clear();
	int count = MIN(p_positions.size(), p_radii.size());
	_wards.reserve(count);
	for (int i = 0; i < count; i++) {
		Ward w;
		w.pos = Vector3(p_positions[i]);
		w.radius = float(p_radii[i]);
		_wards.push_back(w);
	}
	_rebuild_ward_grid();
}

bool SporeManager::is_spore_warded(int32_t p_id) const {
	if (!_alive[p_id] || _wards.is_empty()) {
		return false;
	}

	const Vector3 &spore_pos = _positions[p_id];
	float spore_radius = _radii[p_id];

	Vector3i center_key(
		int(Math::floor(spore_pos.x / WARD_CELL_SIZE)),
		int(Math::floor(spore_pos.y / WARD_CELL_SIZE)),
		int(Math::floor(spore_pos.z / WARD_CELL_SIZE))
	);

	// Search the 27 neighboring ward cells.
	for (int dx = -1; dx <= 1; dx++) {
		for (int dy = -1; dy <= 1; dy++) {
			for (int dz = -1; dz <= 1; dz++) {
				Vector3i key = center_key + Vector3i(dx, dy, dz);
				const Vector<int32_t> *ward_ids = _ward_grid.getptr(key);
				if (ward_ids == nullptr) {
					continue;
				}
				for (int32_t ward_idx : *ward_ids) {
					float dist = spore_pos.distance_to(_wards[ward_idx].pos);
					if (dist < _wards[ward_idx].radius + spore_radius * 0.5f) {
						return true;
					}
				}
			}
		}
	}

	return false;
}

// ---------------------------------------------------------------------------
// Force-limit (ward suppression)
// ---------------------------------------------------------------------------

void SporeManager::set_spore_force_limit(int32_t p_id, float p_limit) {
	if (!_alive[p_id]) {
		return;
	}
	_force_limits.set(p_id, MAX(p_limit, 0.0f));
}

float SporeManager::get_spore_force_limit(int32_t p_id) const {
	if (!_alive[p_id]) {
		return 0.0f;
	}
	return _force_limits[p_id];
}

void SporeManager::set_force_limit_shrink_speed(float p_speed) {
	_force_limit_shrink_speed = MAX(p_speed, 0.0f);
}

float SporeManager::get_force_limit_shrink_speed() const {
	return _force_limit_shrink_speed;
}

// ---------------------------------------------------------------------------
// Per-chamber queries
// ---------------------------------------------------------------------------

PackedFloat32Array SporeManager::get_spore_transforms_for_chamber(int p_chamber_id) const {
	// Each instance is 12 floats: a 3x4 transform matrix (column-major).
	// [col0.x, col0.y, col0.z, origin.x,
	//  col1.x, col1.y, col1.z, origin.y,
	//  col2.x, col2.y, col2.z, origin.z]
	// For a uniform scaled sphere: columns are [R,0,0], [0,R,0], [0,0,R].
	int count = 0;
	for (int32_t id : _alive_ids) {
		if (_chamber_ids[id] == p_chamber_id) {
			count++;
		}
	}

	PackedFloat32Array buffer;
	buffer.resize(count * 12);
	float *ptr = buffer.ptrw();
	int idx = 0;
	for (int32_t id : _alive_ids) {
		if (_chamber_ids[id] != p_chamber_id) {
			continue;
		}
		const Vector3 &pos = _positions[id];
		float r = _radii[id];
		// Column 0: (r, 0, 0), origin.x
		ptr[idx + 0] = r;
		ptr[idx + 1] = 0.0f;
		ptr[idx + 2] = 0.0f;
		ptr[idx + 3] = pos.x;
		// Column 1: (0, r, 0), origin.y
		ptr[idx + 4] = 0.0f;
		ptr[idx + 5] = r;
		ptr[idx + 6] = 0.0f;
		ptr[idx + 7] = pos.y;
		// Column 2: (0, 0, r), origin.z
		ptr[idx + 8] = 0.0f;
		ptr[idx + 9] = 0.0f;
		ptr[idx + 10] = r;
		ptr[idx + 11] = pos.z;
		idx += 12;
	}
	return buffer;
}

int SporeManager::get_spore_count_for_chamber(int p_chamber_id) const {
	int count = 0;
	for (int32_t id : _alive_ids) {
		if (_chamber_ids[id] == p_chamber_id) {
			count++;
		}
	}
	return count;
}

int SporeManager::get_spore_chamber(int32_t p_id) const {
	if (!_alive[p_id]) {
		return -1;
	}
	return _chamber_ids[p_id];
}

// ---------------------------------------------------------------------------
// Cell graph — BFS flood-fill, depth assignment, sweep
// ---------------------------------------------------------------------------

void SporeManager::_init_bfs_neighbors() {
	if (!_bfs_neighbors.is_empty()) {
		return;
	}
	// 5×5×5 Chebyshev-distance-2 neighbourhood (124 offsets).
	_bfs_neighbors.reserve(124);
	for (int x = -2; x <= 2; x++) {
		for (int y = -2; y <= 2; y++) {
			for (int z = -2; z <= 2; z++) {
				if (x != 0 || y != 0 || z != 0) {
					_bfs_neighbors.push_back(Vector3i(x, y, z));
				}
			}
		}
	}
}

void SporeManager::add_cell(const Vector3i &p_grid_key, const Vector3 &p_world_pos, int p_chamber_id) {
	Cell *existing = _cells.getptr(p_grid_key);
	if (existing) {
		// Update chamber_id for boundary cells that belong to a later chamber.
		existing->chamber_id = p_chamber_id;
		existing->world_pos = p_world_pos;
		return;
	}
	Cell c;
	c.grid_key = p_grid_key;
	c.world_pos = p_world_pos;
	c.chamber_id = p_chamber_id;
	c.depth = -1;
	c.spawned = false;
	c.blocked_by_ward = false;
	_cells.insert(p_grid_key, c);
}

void SporeManager::remove_cell(const Vector3i &p_grid_key) {
	_cells.erase(p_grid_key);
	_sweep_dirty = true;
}

bool SporeManager::has_cell(const Vector3i &p_grid_key) const {
	return _cells.has(p_grid_key);
}

void SporeManager::set_chamber_entry_cells(int p_chamber_id, const TypedArray<Vector3i> &p_keys) {
	Vector<Vector3i> keys;
	keys.resize(p_keys.size());
	for (int i = 0; i < p_keys.size(); i++) {
		keys.set(i, Vector3i(p_keys[i]));
	}
	_entry_cells[p_chamber_id] = keys;
}

void SporeManager::set_chamber_exit_cells(int p_chamber_id, const TypedArray<Vector3i> &p_keys) {
	Vector<Vector3i> keys;
	keys.resize(p_keys.size());
	for (int i = 0; i < p_keys.size(); i++) {
		keys.set(i, Vector3i(p_keys[i]));
	}
	_exit_cells[p_chamber_id] = keys;
}

void SporeManager::set_chamber_speed(int p_chamber_id, float p_speed) {
	_chamber_speeds[p_chamber_id] = MAX(p_speed, 0.0f);
}

float SporeManager::get_chamber_speed(int p_chamber_id) const {
	const float *s = _chamber_speeds.getptr(p_chamber_id);
	return s ? *s : 1.0f;
}

// ---- BFS flood-fill ----

void SporeManager::_run_bfs_incremental(float p_target_depth) {
	if (_cells.is_empty()) {
		return;
	}

	_init_bfs_neighbors();

	// Collect seeds: entry cells (depth 0) and frontier cells
	// at depths ≤ p_target_depth that are already visited.
	// BFS processes one wave at a time until all cells with
	// depth ≤ p_target_depth are assigned.
	struct BfsFrontier {
		Vector3i key;
		int depth;
	};
	Vector<BfsFrontier> wave;
	HashSet<Vector3i> visited;

	// Seed from the manual start cell (if set) at depth 0.
	// This takes priority over entry-cell seeding — used when the
	// first chamber has no level_in_pos (e.g. chamber_0 is pre-placed).
	if (_has_start_cell) {
		Cell *sc = _cells.getptr(_start_cell);
		if (sc && !sc->blocked_by_ward) {
			sc->depth = 0;
			wave.push_back({ _start_cell, 0 });
			visited.insert(_start_cell);
		}
	}

	// Seed from entry cells of the first non-empty chamber.
	// Subsequent chambers are reached by the BFS flowing through
	// the connected cell graph from previous chambers.
	if (!_has_start_cell || wave.is_empty()) {
		// Collect chambers in sorted order.
		Vector<int32_t> chambers;
		for (const auto &E : _entry_cells) {
			chambers.push_back(E.key);
		}
		chambers.sort();

		for (int32_t ch : chambers) {
			const Vector<Vector3i> *entries = _entry_cells.getptr(ch);
			if (!entries || entries->is_empty()) {
				continue;
			}
			for (const Vector3i &key : *entries) {
				Cell *c = _cells.getptr(key);
				if (!c || c->blocked_by_ward) {
					continue;
				}
				c->depth = 0;
				wave.push_back({ key, 0 });
				visited.insert(key);
			}
			break; // Only seed from the first non-empty chamber.
		}
	}

	// Also seed from existing frontier cells (already-visited cells
	// that sit at the edge of the computed region).  These carry
	// their existing depth forward so the new BFS extends outward.
	{
		for (const auto &E_scan : _cells) {
			const Vector3i &key = E_scan.key;
			const Cell &cell_ref = E_scan.value;
			if (cell_ref.blocked_by_ward || cell_ref.depth < 0) {
				continue;
			}
			if (cell_ref.depth > (int)p_target_depth) {
				continue; // beyond target, not yet needed
			}
			if (visited.has(key)) {
				continue; // already seeded from entries
			}
			// Check if this cell is at the frontier — it has at least
			// one unvisited neighbour, or it's newly unblocked.
			bool is_frontier = false;
			for (const Vector3i &n : _bfs_neighbors) {
				Vector3i nk = key + n;
				const Cell *nc = _cells.getptr(nk);
				if (nc && !nc->blocked_by_ward && nc->depth < 0) {
					is_frontier = true;
					break;
				}
			}
			if (is_frontier) {
				wave.push_back({ key, cell_ref.depth });
				visited.insert(key);
			}
		}
	}

	if (wave.is_empty()) {
		return;
	}

	// Run BFS waves until we hit p_target_depth or run out of cells.
	while (!wave.is_empty()) {
		Vector<BfsFrontier> next_wave;
		bool reached_target = true;

		for (const BfsFrontier &f : wave) {
			int nd = f.depth + 1;
			if (nd > (int)p_target_depth) {
				reached_target = false;
				continue;
			}

			for (const Vector3i &n : _bfs_neighbors) {
				Vector3i nk = f.key + n;
				if (visited.has(nk)) {
					continue;
				}
				Cell *c = _cells.getptr(nk);
				if (!c || c->blocked_by_ward) {
					continue;
				}
				if (c->depth < 0 || c->depth > nd) {
					c->depth = nd;
					visited.insert(nk);
					next_wave.push_back({ nk, nd });
				}
			}
		}

		wave = next_wave;
		if (reached_target && next_wave.is_empty()) {
			break;
		}
	}

	_bfs_computed_depth = p_target_depth;
	_sweep_dirty = true;
}

void SporeManager::_build_sweep_list() {
	_sorted_cells.clear();

	// Bucket cells by depth (depths are small integers, so bucket sort is fast).
	// We use a HashMap so we don't need to find max_depth upfront.
	HashMap<int, Vector<Vector3i>> buckets;
	int min_depth = 0x7FFFFFFF;
	int max_depth = -1;

	for (const auto &E : _cells) {
		if (E.value.depth >= 0 && !E.value.blocked_by_ward) {
			int d = E.value.depth;
			buckets[d].push_back(E.key);
			if (d < min_depth) {
				min_depth = d;
			}
			if (d > max_depth) {
				max_depth = d;
			}
		}
	}

	// Flatten buckets in depth order.
	for (int d = min_depth; d <= max_depth; d++) {
		const Vector<Vector3i> *bucket = buckets.getptr(d);
		if (bucket) {
			for (const Vector3i &key : *bucket) {
				_sorted_cells.push_back(key);
			}
		}
	}

	// Reset sweep cursor if this is a fresh build.
	if (_sweep_idx <= 0 || _sweep_idx > _sorted_cells.size()) {
		_sweep_idx = 0;
		_sweep = 0.0f;
	}
	_sweep_dirty = false;
}

void SporeManager::propagate_depths() {
	// Reset all cells to unvisited.
	for (auto &E : _cells) {
		E.value.depth = -1;
	}

	// Run BFS to a generous depth (covers all connected cells).
	// Use INT_MAX / 2 to avoid overflow — in practice no chamber
	// has cells deeper than a few hundred.
	_run_bfs_incremental(10000.0f);

	// Reset sweep cursor since the sorted list is being rebuilt.
	// Old spawned cells will be skipped by the spawned check in
	// advance_sweeps; the sweep will "rush" through them and pick
	// up at the first unspawned cell.
	_sweep = 0.0f;
	_sweep_idx = 0;

	// Build the sorted sweep list.
	_build_sweep_list();

	// Compute per-chamber speeds if not already set.
	// Speed = (median_exit_depth - 1) / consume_time, but consume_time
	// is converted to speed on the GDScript side before calling
	// set_chamber_speed().  We only compute defaults here.
	for (const auto &E : _exit_cells) {
		int chamber_id = E.key;
		if (_chamber_speeds.has(chamber_id)) {
			continue; // already set by GDScript
		}
		// Default: sweep takes ~30 depth units.
		float speed = 1.0f;
		_chamber_speeds[chamber_id] = speed;
	}
}

// ---- Sweep advance ----

Dictionary SporeManager::advance_sweeps(float p_delta) {
	Dictionary result;

	if (_cells.is_empty()) {
		return result;
	}

	// Lazy BFS: if the sweep is close to computed_max_depth,
	// run another incremental wave ahead.
	if (_sweep + BFS_LOOKAHEAD * 0.5f > _bfs_computed_depth) {
		_run_bfs_incremental(_bfs_computed_depth + BFS_LOOKAHEAD);
	}

	if (_sweep_dirty) {
		_build_sweep_list();
	}

	if (_sorted_cells.is_empty()) {
		return result;
	}

	// Determine current speed from the chamber of the next cell.
	float speed = 1.0f;
	if (_sweep_idx < _sorted_cells.size()) {
		const Cell *c = _cells.getptr(_sorted_cells[_sweep_idx]);
		if (c) {
			const float *s = _chamber_speeds.getptr(c->chamber_id);
			speed = s ? *s : 1.0f;
		}
	}

	_sweep += speed * p_delta;

	// Activate all cells whose depth ≤ sweep, respecting ward blocking.
	while (_sweep_idx < _sorted_cells.size()) {
		const Vector3i &key = _sorted_cells[_sweep_idx];
		Cell *c = _cells.getptr(key);
		if (!c) {
			_sweep_idx++;
			continue;
		}

		// Skip cells blocked by wards (they were unblocked after a
		// re-BFS but may still be in the stale _sorted_cells list).
		if (c->blocked_by_ward) {
			_sweep_idx++;
			continue;
		}

		// Update speed if this cell belongs to a different chamber
		// than the previous one.
		if (_sweep_idx > 0 && _sweep_idx - 1 < _sorted_cells.size()) {
			const Cell *prev = _cells.getptr(_sorted_cells[_sweep_idx - 1]);
			if (prev && prev->chamber_id != c->chamber_id) {
				const float *s = _chamber_speeds.getptr(c->chamber_id);
				speed = s ? *s : 1.0f;
			}
		}

		if ((float)c->depth > _sweep) {
			break;
		}

		_sweep_idx++;

		if (c->spawned) {
			continue;
		}

		// Collect into per-chamber result arrays.
		int ch = c->chamber_id;
		Array arr;
		if (result.has(ch)) {
			arr = result[ch];
		}
		arr.push_back(key);
		result[ch] = arr;
	}

	return result;
}

void SporeManager::mark_cell_spawned(const Vector3i &p_grid_key) {
	Cell *c = _cells.getptr(p_grid_key);
	if (c) {
		c->spawned = true;
	}
}

void SporeManager::on_wards_changed() {
	// Mark cells as blocked/unblocked based on current ward positions.
	// Then re-run BFS so blocked cells get unreachable depths.
	if (_wards.is_empty()) {
		// No wards: unblock all cells.
		for (auto &E : _cells) {
			E.value.blocked_by_ward = false;
		}
	} else {
		// Check every cell against all wards.
		for (auto &E : _cells) {
			bool blocked = false;
			Vector3 pos = E.value.world_pos;
			for (const Ward &w : _wards) {
				if (pos.distance_to(w.pos) < w.radius) {
					blocked = true;
					break;
				}
			}
			E.value.blocked_by_ward = blocked;
		}
	}

	// Reset depths and re-run BFS from the current sweep frontier.
	// Cells with depth > _sweep get reset to -1 so the BFS re-assigns them.
	// Cells with depth ≤ _sweep (already swept past) keep their depth
	// and act as BFS seeds.
	for (auto &E : _cells) {
		if (E.value.blocked_by_ward) {
			E.value.depth = -1; // remove from sweep
		} else if (E.value.depth > (int)_sweep) {
			E.value.depth = -1; // reset for re-BFS
		}
		// Cells with depth ≤ _sweep keep their depth (already activated).
	}

	// Re-run BFS to fill in the reset cells.
	_run_bfs_incremental(_bfs_computed_depth > 0 ? _bfs_computed_depth : BFS_LOOKAHEAD);
	_sweep_dirty = true;
}

// ---- GDScript queries ----

int SporeManager::get_cell_depth(const Vector3i &p_grid_key) const {
	const Cell *c = _cells.getptr(p_grid_key);
	return c ? c->depth : -1;
}

int SporeManager::get_cell_chamber(const Vector3i &p_grid_key) const {
	const Cell *c = _cells.getptr(p_grid_key);
	return c ? c->chamber_id : -1;
}

bool SporeManager::is_cell_blocked(const Vector3i &p_grid_key) const {
	const Cell *c = _cells.getptr(p_grid_key);
	return c ? c->blocked_by_ward : false;
}

void SporeManager::set_spore_res(float p_res) {
	_spore_res = MAX(p_res, 0.01f);
}

float SporeManager::get_spore_res() const {
	return _spore_res;
}

void SporeManager::set_start_cell(const Vector3i &p_grid_key) {
	_start_cell = p_grid_key;
	_has_start_cell = true;
}

int SporeManager::get_bfs_neighbor_count() const {
	return _bfs_neighbors.size();
}

// ---------------------------------------------------------------------------
// Bind methods
// ---------------------------------------------------------------------------

void SporeManager::_bind_methods() {
	// Enums
	BIND_ENUM_CONSTANT(STATE_DEAD);
	BIND_ENUM_CONSTANT(STATE_START_DELAY);
	BIND_ENUM_CONSTANT(STATE_CONNECTING);
	BIND_ENUM_CONSTANT(STATE_ACTIVE);
	BIND_ENUM_CONSTANT(STATE_DYING);

	BIND_ENUM_CONSTANT(PROFILE_NORMAL);
	BIND_ENUM_CONSTANT(PROFILE_STRAIN);

	// Lifecycle
	ClassDB::bind_method(D_METHOD("add_spore", "position", "profile", "chamber_id"), &SporeManager::add_spore, DEFVAL(PROFILE_NORMAL), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("remove_spore", "id"), &SporeManager::remove_spore);
	ClassDB::bind_method(D_METHOD("set_spore_state", "id", "state"), &SporeManager::set_spore_state);
	ClassDB::bind_method(D_METHOD("get_spore_state", "id"), &SporeManager::get_spore_state);
	ClassDB::bind_method(D_METHOD("set_spore_profile", "id", "profile"), &SporeManager::set_spore_profile);
	ClassDB::bind_method(D_METHOD("get_spore_profile", "id"), &SporeManager::get_spore_profile);
	ClassDB::bind_method(D_METHOD("set_start_delay", "delay"), &SporeManager::set_start_delay);
	ClassDB::bind_method(D_METHOD("get_start_delay"), &SporeManager::get_start_delay);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "start_delay"), "set_start_delay", "get_start_delay");

	// Update
	ClassDB::bind_method(D_METHOD("update", "delta", "total_time"), &SporeManager::update);

	// Getters
	ClassDB::bind_method(D_METHOD("get_spore_radius", "id"), &SporeManager::get_spore_radius);
	ClassDB::bind_method(D_METHOD("get_spore_position", "id"), &SporeManager::get_spore_position);
	ClassDB::bind_method(D_METHOD("is_spore_alive", "id"), &SporeManager::is_spore_alive);
	ClassDB::bind_method(D_METHOD("get_spore_count"), &SporeManager::get_spore_count);
	ClassDB::bind_method(D_METHOD("get_active_spore_count"), &SporeManager::get_active_spore_count);

	// Spatial queries
	ClassDB::bind_method(D_METHOD("query_nearby", "position"), &SporeManager::query_nearby);
	ClassDB::bind_method(D_METHOD("query_sphere", "center", "radius"), &SporeManager::query_sphere);
	ClassDB::bind_method(D_METHOD("query_spores_in_range", "position", "radius"), &SporeManager::query_spores_in_range);
	ClassDB::bind_method(D_METHOD("is_any_spore_in_range", "center", "radius"), &SporeManager::is_any_spore_in_range);
	ClassDB::bind_method(D_METHOD("is_any_dangerous_spore_in_range", "center", "radius"), &SporeManager::is_any_dangerous_spore_in_range);

	// Wards
	ClassDB::bind_method(D_METHOD("set_wards", "positions", "radii"), &SporeManager::set_wards);
	ClassDB::bind_method(D_METHOD("is_spore_warded", "id"), &SporeManager::is_spore_warded);

	// Force-limit
	ClassDB::bind_method(D_METHOD("set_spore_force_limit", "id", "limit"), &SporeManager::set_spore_force_limit);
	ClassDB::bind_method(D_METHOD("get_spore_force_limit", "id"), &SporeManager::get_spore_force_limit);
	ClassDB::bind_method(D_METHOD("set_force_limit_shrink_speed", "speed"), &SporeManager::set_force_limit_shrink_speed);
	ClassDB::bind_method(D_METHOD("get_force_limit_shrink_speed"), &SporeManager::get_force_limit_shrink_speed);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "force_limit_shrink_speed"), "set_force_limit_shrink_speed", "get_force_limit_shrink_speed");

	// Per-chamber
	ClassDB::bind_method(D_METHOD("remove_spores_in_chamber", "chamber_id"), &SporeManager::remove_spores_in_chamber);
	ClassDB::bind_method(D_METHOD("get_spore_transforms_for_chamber", "chamber_id"), &SporeManager::get_spore_transforms_for_chamber);
	ClassDB::bind_method(D_METHOD("get_spore_count_for_chamber", "chamber_id"), &SporeManager::get_spore_count_for_chamber);
	ClassDB::bind_method(D_METHOD("get_spore_chamber", "id"), &SporeManager::get_spore_chamber);

	// Cell graph
	ClassDB::bind_method(D_METHOD("add_cell", "grid_key", "world_pos", "chamber_id"), &SporeManager::add_cell);
	ClassDB::bind_method(D_METHOD("remove_cell", "grid_key"), &SporeManager::remove_cell);
	ClassDB::bind_method(D_METHOD("has_cell", "grid_key"), &SporeManager::has_cell);
	ClassDB::bind_method(D_METHOD("set_chamber_entry_cells", "chamber_id", "keys"), &SporeManager::set_chamber_entry_cells);
	ClassDB::bind_method(D_METHOD("set_chamber_exit_cells", "chamber_id", "keys"), &SporeManager::set_chamber_exit_cells);
	ClassDB::bind_method(D_METHOD("set_chamber_speed", "chamber_id", "speed"), &SporeManager::set_chamber_speed);
	ClassDB::bind_method(D_METHOD("get_chamber_speed", "chamber_id"), &SporeManager::get_chamber_speed);
	ClassDB::bind_method(D_METHOD("propagate_depths"), &SporeManager::propagate_depths);
	ClassDB::bind_method(D_METHOD("advance_sweeps", "delta"), &SporeManager::advance_sweeps);
	ClassDB::bind_method(D_METHOD("mark_cell_spawned", "grid_key"), &SporeManager::mark_cell_spawned);
	ClassDB::bind_method(D_METHOD("on_wards_changed"), &SporeManager::on_wards_changed);
	ClassDB::bind_method(D_METHOD("get_cell_depth", "grid_key"), &SporeManager::get_cell_depth);
	ClassDB::bind_method(D_METHOD("get_cell_chamber", "grid_key"), &SporeManager::get_cell_chamber);
	ClassDB::bind_method(D_METHOD("is_cell_blocked", "grid_key"), &SporeManager::is_cell_blocked);
	ClassDB::bind_method(D_METHOD("set_spore_res", "res"), &SporeManager::set_spore_res);
	ClassDB::bind_method(D_METHOD("get_spore_res"), &SporeManager::get_spore_res);
	ClassDB::bind_method(D_METHOD("set_start_cell", "grid_key"), &SporeManager::set_start_cell);
	ClassDB::bind_method(D_METHOD("get_bfs_neighbor_count"), &SporeManager::get_bfs_neighbor_count);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spore_res"), "set_spore_res", "get_spore_res");
}
