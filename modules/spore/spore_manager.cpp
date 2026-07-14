/**************************************************************************/
/*  spore_manager.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "spore_manager.h"

#include <algorithm>
#include <vector>

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
		_spawn_depths.push_back(-1.0f);
		_radii.push_back(0.0001f);
		_force_limits.push_back(0.0f);
		_seed_offsets.push_back(0.0f);
		_states.push_back(STATE_DEAD);
		_profiles.push_back(PROFILE_NORMAL);
		_chamber_ids.push_back(-1);
		_alive.push_back(false);
		_shrink_times.push_back(-1.0f);
		_shrink_start_radii.push_back(0.0f);
		return id;
	}
	int32_t id = _free_list[_free_list.size() - 1];
	_free_list.resize(_free_list.size() - 1);
	return id;
}

void SporeManager::_free_id(int32_t p_id) {
	_alive.set(p_id, false);
	_states.set(p_id, STATE_DEAD);
	_shrink_times.set(p_id, -1.0f);
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

int32_t SporeManager::add_spore(const Vector3 &p_pos, int p_profile, int p_chamber_id, float p_spawn_depth) {
	int32_t id = _allocate_id();
	_positions.set(id, p_pos);
	_spawn_times.set(id, -1.0f); // Will be set by the first update() call.
	_spawn_depths.set(id, p_spawn_depth);
	_radii.set(id, 0.0001f);     // Reset stale radius from recycled ID.
	_force_limits.set(id, 0.0f); // Reset stale force-limit from recycled ID.
	_shrink_times.set(id, -1.0f); // Reset stale shrink state from recycled ID.
	_shrink_start_radii.set(id, 0.0f);
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

int32_t SporeManager::add_spore_with_time(const Vector3 &p_pos, int p_profile, int p_chamber_id, float p_spawn_depth, double p_spawn_time) {
	int32_t id = _allocate_id();
	_positions.set(id, p_pos);
	_spawn_times.set(id, (float)p_spawn_time);
	_spawn_depths.set(id, p_spawn_depth);
	_radii.set(id, 0.0001f);     // Reset stale radius from recycled ID; update() will grow it.
	_force_limits.set(id, 0.0f); // Reset stale force-limit from recycled ID.
	_shrink_times.set(id, -1.0f); // Reset stale shrink state from recycled ID.
	_shrink_start_radii.set(id, 0.0f);
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

void SporeManager::remove_spores_in_chamber(int p_chamber_id, bool p_shrink_first) {
	if (p_shrink_first) {
		// Transition to shrinking — update() will shrink them over
		// _overlap_shrink_duration seconds and then free them.
		shrink_spores_in_chamber(p_chamber_id);
		return;
	}

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

void SporeManager::shrink_spore(int32_t p_id) {
	if (!_alive[p_id]) {
		return;
	}
	// Don't double-shrink.
	if (_states[p_id] == STATE_SHRINKING || _states[p_id] == STATE_DEAD) {
		return;
	}
	_shrink_times.set(p_id, _last_total_time);
	_shrink_start_radii.set(p_id, _radii[p_id]);
	_states.set(p_id, STATE_SHRINKING);
}

void SporeManager::shrink_spores_in_chamber(int p_chamber_id) {
	for (int32_t id : _alive_ids) {
		if (_chamber_ids[id] == p_chamber_id) {
			shrink_spore(id);
		}
	}
}

void SporeManager::_detect_overlaps(double p_total_time) {
	// Group mature spores by chamber so O(n²) only runs per-chamber.
	HashMap<int, Vector<int32_t>> chamber_spores;
	for (int32_t id : _alive_ids) {
		if (_states[id] == STATE_MATURE) {
			chamber_spores[_chamber_ids[id]].push_back(id);
		}
	}

	for (const auto &E : chamber_spores) {
		const Vector<int32_t> &spores = E.value;
		if (spores.size() < _overlap_min_count) {
			continue;
		}

		// Find overlapping pairs: two mature spores whose centres are
		// closer than _overlap_radius are considered redundant.
		struct Pair {
			int32_t a;
			int32_t b;
		};
		Vector<Pair> pairs;

		for (int i = 0; i < spores.size(); i++) {
			for (int j = i + 1; j < spores.size(); j++) {
				int32_t a_id = spores[i];
				int32_t b_id = spores[j];
				float dist = _positions[a_id].distance_to(_positions[b_id]);
				if (dist < _overlap_radius) {
					pairs.push_back({ a_id, b_id });
				}
			}
		}

		if (pairs.is_empty()) {
			continue;
		}

		// Mark a fraction of overlapping spores for shrinking.
		// Always pick the smaller spore from each pair as the victim.
		int to_remove = MAX(1, int(pairs.size() * _overlap_shrink_fraction));
		HashSet<int32_t> marked;
		for (int i = 0; i < pairs.size() && marked.size() < to_remove; i++) {
			int32_t victim = (_radii[pairs[i].a] < _radii[pairs[i].b]) ? pairs[i].a : pairs[i].b;
			if (!marked.has(victim)) {
				marked.insert(victim);
				shrink_spore(victim);
			}
		}
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
		float effective_elapsed = elapsed;
		if (_depth_lifecycle_enabled && _spawn_depths[id] >= 0.0f && _sweep >= _spawn_depths[id] + 0.001f) {
			float depth_gap = _sweep - _spawn_depths[id];
			if (depth_gap >= _depth_lifecycle_full_threshold) {
				uint8_t st = _states[id];
				if (st == STATE_ACTIVE || st == STATE_START_DELAY || st == STATE_CONNECTING) {
					_states.set(id, STATE_MATURE);
				}
				effective_elapsed = elapsed + 10000.0f;
			} else if (depth_gap >= _depth_lifecycle_mid_threshold) {
				float span = MAX(_depth_lifecycle_full_threshold - _depth_lifecycle_mid_threshold, 0.001f);
				float blend = CLAMP((depth_gap - _depth_lifecycle_mid_threshold) / span, 0.0f, 1.0f);
				float accel = 1.0f + blend * MAX(_depth_lifecycle_mid_multiplier - 1.0f, 0.0f);
				effective_elapsed = elapsed * accel;
			}
		}

		// ---- Shrinking spores (visual removal) ----
		// Radius shrinks toward zero so the spore recedes visually.
		// When fully shrunk the spore is freed — deterministic on
		// both host and client because shrink times and durations
		// are identical.
		if (_states[id] == STATE_SHRINKING) {
			float shrink_elapsed = float(p_total_time - _shrink_times[id]);
			float t = MIN(shrink_elapsed / _overlap_shrink_duration, 1.0f);
			float new_radius = _shrink_start_radii[id] * (1.0f - t);

			// Update grid if level changed.
			float old_radius = _radii[id];
			int old_level = SporeGrid::_level_for_radius(old_radius);
			int new_level = SporeGrid::_level_for_radius(new_radius);
			_radii.set(id, new_radius);
			if (old_level != new_level) {
				_grid.migrate(id, _positions[id], old_radius, _positions[id], new_radius);
			}

			// Free when fully shrunk.
			if (t >= 1.0f) {
				_grid.remove(id);
				_free_id(id);
			}
			continue;
		}

		// ---- Lifetime death (disabled when mature phase is on) ----
		if (!_mature_phase_enabled) {
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
		}

		float old_radius = _radii[id];
		float new_radius = _compute_radius(effective_elapsed, _profiles[id], _seed_offsets[id]);

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

		// ---- Transition to mature phase ----
		// When a spore has completed all growth phases (Phase 4: at cap
		// with subtle pulse), mark it as mature.  Only spores in ACTIVE
		// or START_DELAY (which moved through CONNECTING→ACTIVE in
		// GDScript) are eligible; DYING and SHRINKING are excluded.
		if (_mature_phase_enabled) {
			uint8_t st = _states[id];
			if (st == STATE_ACTIVE || st == STATE_START_DELAY || st == STATE_CONNECTING) {
				// Check if we've reached Phase 4 (past all three growth phases).
				float growth_elapsed = effective_elapsed - _start_delay;
				if (growth_elapsed >= PHASE1_DURATION + PHASE2_DURATION + PHASE3_DURATION) {
					_states.set(id, STATE_MATURE);
				}
			}
		}
	}

	// ---- Periodic overlap cleanup ----
	if (_overlap_cleanup_enabled && _mature_phase_enabled) {
		_overlap_timer += float(p_delta);
		if (_overlap_timer >= _overlap_interval) {
			_overlap_timer = 0.0f;
			_detect_overlaps(p_total_time);
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

// ---- Depth-based lifecycle acceleration config ----

void SporeManager::set_depth_lifecycle_enabled(bool p_enabled) {
	_depth_lifecycle_enabled = p_enabled;
}

bool SporeManager::is_depth_lifecycle_enabled() const {
	return _depth_lifecycle_enabled;
}

void SporeManager::set_depth_lifecycle_mid_threshold(float p_threshold) {
	_depth_lifecycle_mid_threshold = MAX(p_threshold, 0.0f);
	_depth_lifecycle_full_threshold = MAX(_depth_lifecycle_full_threshold, _depth_lifecycle_mid_threshold);
}

float SporeManager::get_depth_lifecycle_mid_threshold() const {
	return _depth_lifecycle_mid_threshold;
}

void SporeManager::set_depth_lifecycle_full_threshold(float p_threshold) {
	_depth_lifecycle_full_threshold = MAX(p_threshold, _depth_lifecycle_mid_threshold);
}

float SporeManager::get_depth_lifecycle_full_threshold() const {
	return _depth_lifecycle_full_threshold;
}

void SporeManager::set_depth_lifecycle_mid_multiplier(float p_multiplier) {
	_depth_lifecycle_mid_multiplier = MAX(p_multiplier, 1.0f);
}

float SporeManager::get_depth_lifecycle_mid_multiplier() const {
	return _depth_lifecycle_mid_multiplier;
}

// ---- Mature phase & overlap cleanup config ----

void SporeManager::set_mature_phase_enabled(bool p_enabled) {
	_mature_phase_enabled = p_enabled;
}

bool SporeManager::is_mature_phase_enabled() const {
	return _mature_phase_enabled;
}

void SporeManager::set_overlap_cleanup_enabled(bool p_enabled) {
	_overlap_cleanup_enabled = p_enabled;
}

bool SporeManager::is_overlap_cleanup_enabled() const {
	return _overlap_cleanup_enabled;
}

void SporeManager::set_overlap_shrink_fraction(float p_fraction) {
	_overlap_shrink_fraction = CLAMP(p_fraction, 0.0f, 1.0f);
}

float SporeManager::get_overlap_shrink_fraction() const {
	return _overlap_shrink_fraction;
}

void SporeManager::set_overlap_shrink_duration(float p_duration) {
	_overlap_shrink_duration = MAX(p_duration, 0.1f);
}

float SporeManager::get_overlap_shrink_duration() const {
	return _overlap_shrink_duration;
}

void SporeManager::set_overlap_radius(float p_radius) {
	_overlap_radius = MAX(p_radius, 0.5f);
}

float SporeManager::get_overlap_radius() const {
	return _overlap_radius;
}

void SporeManager::set_overlap_interval(float p_interval) {
	_overlap_interval = MAX(p_interval, 0.5f);
	_overlap_timer = 0.0f; // reset accumulator so next pass uses new interval
}

float SporeManager::get_overlap_interval() const {
	return _overlap_interval;
}

void SporeManager::set_overlap_min_count(int p_count) {
	_overlap_min_count = MAX(p_count, 1);
}

int SporeManager::get_overlap_min_count() const {
	return _overlap_min_count;
}

// ---- Depth noise ----

void SporeManager::set_depth_noise_amplitude(float p_amplitude) {
	_depth_noise_amplitude = MAX(p_amplitude, 0.0f);
	_sweep_dirty = true; // re-sort on next advance
}

float SporeManager::get_depth_noise_amplitude() const {
	return _depth_noise_amplitude;
}

void SporeManager::set_depth_noise_frequency(float p_frequency) {
	// Frequency changes require recomputing all cell noises and resorting.
	_depth_noise_frequency = MAX(p_frequency, 0.001f);
	for (auto &E : _cells) {
		Cell &c = E.value;
		float freq = _depth_noise_frequency;
		float fx = (float)c.grid_key.x * freq;
		float fy = (float)c.grid_key.y * freq;
		float fz = (float)c.grid_key.z * freq;
		float n = Math::sin(fx * 1.271f + fy * 3.117f + fz * 0.747f) * 0.6f;
		n += Math::sin(fx * 2.695f + fy * 1.833f + fz * 4.219f) * 0.4f;
		c.depth_noise = CLAMP(n, -1.0f, 1.0f);
	}
	_sweep_dirty = true;
}

float SporeManager::get_depth_noise_frequency() const {
	return _depth_noise_frequency;
}

// ---- Spore handicap (rubber-banding) ----

void SporeManager::set_player_chamber(int p_chamber) {
	_player_chamber = p_chamber;
}

float SporeManager::get_spore_handicap() const {
	return _spore_handicap;
}

void SporeManager::set_catch_up_threshold(int p_threshold) {
	_catch_up_threshold = MAX(p_threshold, 0);
}

int SporeManager::get_catch_up_threshold() const {
	return _catch_up_threshold;
}

void SporeManager::set_catch_up_speed_multiplier(float p_multiplier) {
	_catch_up_speed_multiplier = MAX(p_multiplier, 1.0f);
}

float SporeManager::get_catch_up_speed_multiplier() const {
	return _catch_up_speed_multiplier;
}

void SporeManager::set_payback_speed_multiplier(float p_multiplier) {
	_payback_speed_multiplier = CLAMP(p_multiplier, 0.0f, 1.0f);
}

float SporeManager::get_payback_speed_multiplier() const {
	return _payback_speed_multiplier;
}

// ---- Chamber 0 speed boost ----

void SporeManager::set_chamber_zero_speed_multiplier(float p_multiplier) {
	_chamber_zero_speed_multiplier = MAX(p_multiplier, 0.0f);
}

float SporeManager::get_chamber_zero_speed_multiplier() const {
	return _chamber_zero_speed_multiplier;
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

	// Deterministic positional noise for frontier waviness.
	// Two-octave sine hash produces smooth variation in [-1, 1].
	// Multiplied by _depth_noise_amplitude at sort/activation time
	// so amplitude changes take effect immediately.
	{
		float freq = MAX(_depth_noise_frequency, 0.001f);
		float fx = (float)p_grid_key.x * freq;
		float fy = (float)p_grid_key.y * freq;
		float fz = (float)p_grid_key.z * freq;
		float n = Math::sin(fx * 1.271f + fy * 3.117f + fz * 0.747f) * 0.6f;
		n += Math::sin(fx * 2.695f + fy * 1.833f + fz * 4.219f) * 0.4f;
		c.depth_noise = CLAMP(n, -1.0f, 1.0f);
	}

	_cells.insert(p_grid_key, c);
	_cells_added = true;

	// New cell may give existing visited neighbours a new unvisited
	// neighbour.  Add those neighbours to the frontier set so the
	// next BFS extension can seed from them directly.
	_init_bfs_neighbors();
	for (const Vector3i &n : _bfs_neighbors) {
		Vector3i nk = p_grid_key + n;
		const Cell *nc = _cells.getptr(nk);
		if (nc && !nc->blocked_by_ward && nc->depth >= 0) {
			_frontier_set.insert(nk);
		}
	}
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

	// Seed from the incrementally maintained frontier set.
	// This replaces the old O(cells × 124) full scan — we only check
	// cells that are KNOWN to have unvisited neighbours.
	if (!_frontier_set.is_empty()) {
		for (const Vector3i &key : _frontier_set) {
			if (visited.has(key)) {
				continue;
			}
			const Cell *cell_ref = _cells.getptr(key);
			if (!cell_ref || cell_ref->blocked_by_ward || cell_ref->depth < 0) {
				continue;
			}
			if (cell_ref->depth > (int)p_target_depth) {
				continue;
			}
			// Verify it still has at least one unvisited neighbour
			// (ward changes may have invalidated it).
			bool still_frontier = false;
			for (const Vector3i &n : _bfs_neighbors) {
				Vector3i nk = key + n;
				const Cell *nc = _cells.getptr(nk);
				if (nc && !nc->blocked_by_ward && nc->depth < 0) {
					still_frontier = true;
					break;
				}
			}
			if (still_frontier) {
				wave.push_back({ key, cell_ref->depth });
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

	// Rebuild _frontier_set from the visited set (cells touched in
	// this BFS run only, NOT the whole cell graph).  Any visited cell
	// that still has an unvisited neighbour is a frontier candidate.
	_frontier_set.clear();
	for (const Vector3i &key : visited) {
		const Cell *cell_ref = _cells.getptr(key);
		if (!cell_ref || cell_ref->blocked_by_ward || cell_ref->depth < 0) {
			continue;
		}
		for (const Vector3i &n : _bfs_neighbors) {
			Vector3i nk = key + n;
			const Cell *nc = _cells.getptr(nk);
			if (nc && !nc->blocked_by_ward && nc->depth < 0) {
				_frontier_set.insert(key);
				break;
			}
		}
	}

	print_line(vformat("SporeManager::_run_bfs_incremental  target=%.1f  assigned=%d  frontier=%d  sweep=%.1f",
		p_target_depth, visited.size(), _frontier_set.size(), _sweep));
}

void SporeManager::_build_sweep_list() {
	_sorted_cells.clear();

	float amp = _depth_noise_amplitude;

	// Collect cells with their effective depth (depth + noise * amplitude).
	// Use std::vector + std::sort instead of bucket sort because effective
	// depths are floating-point values.
	std::vector<std::pair<float, Vector3i>> entries;
	entries.reserve(_cells.size());

	for (const auto &E : _cells) {
		if (E.value.depth >= 0 && !E.value.blocked_by_ward) {
			float effective = (float)E.value.depth + E.value.depth_noise * amp;
			entries.emplace_back(effective, E.key);
		}
	}

	std::sort(entries.begin(), entries.end(),
		[](const auto &a, const auto &b) { return a.first < b.first; });

	for (const auto &e : entries) {
		_sorted_cells.push_back(e.second);
	}

	// Skip past already-spawned cells so the sweep doesn't waste time
	// re-traversing them after a re-sync (e.g. when new chambers are added).
	_sweep_idx = 0;
	while (_sweep_idx < _sorted_cells.size()) {
		const Cell *c = _cells.getptr(_sorted_cells[_sweep_idx]);
		if (!c || !c->spawned) {
			break;
		}
		_sweep_idx++;
	}
	// Set sweep to the effective depth of the first unspawned cell.
	_sweep = 0.0f;
	if (_sweep_idx < _sorted_cells.size()) {
		const Cell *c = _cells.getptr(_sorted_cells[_sweep_idx]);
		if (c) {
			_sweep = (float)c->depth + c->depth_noise * amp;
		}
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

	// Build the sorted sweep list (skips already-spawned cells).
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

	// Lazy BFS: if new cells were added since last sweep, force one
	// incremental BFS extension so they get discovered.  Otherwise,
	// run when the sweep is close to computed_max_depth.
	if (_cells_added) {
		float target = MAX(_bfs_computed_depth + BFS_LOOKAHEAD, _sweep + BFS_LOOKAHEAD * 2);
		_run_bfs_incremental(target);
		_cells_added = false;
	} else if (_sweep + BFS_LOOKAHEAD * 0.5f > _bfs_computed_depth) {
		// Always extend far enough ahead of the sweep cursor so the
		// BFS actually reaches the frontier even if _bfs_computed_depth
		// was left stale by a ward change or other edge case.
		_run_bfs_incremental(MAX(_bfs_computed_depth + BFS_LOOKAHEAD, _sweep + BFS_LOOKAHEAD * 2));
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
			if (c->chamber_id == 0) {
				speed *= _chamber_zero_speed_multiplier;
			}
		}
	}

	// ---- Spore handicap (rubber-banding) ----
	// Determine the spore front's chamber from the next unspawned cell.
	int spore_front_chamber = -1;
	if (_sweep_idx < _sorted_cells.size()) {
		const Cell *fc = _cells.getptr(_sorted_cells[_sweep_idx]);
		if (fc) {
			spore_front_chamber = fc->chamber_id;
		}
	}

	float effective_speed = speed;

	if (_spore_handicap > 0.001f && _player_chamber >= 0 &&
			_player_chamber <= spore_front_chamber + _catch_up_threshold) {
		// Payback: slow down spores and drain handicap.
		effective_speed = speed * _payback_speed_multiplier;
		_spore_handicap -= speed * (1.0f - _payback_speed_multiplier) * p_delta;
		if (_spore_handicap < 0.0f) {
			_spore_handicap = 0.0f;
		}
	} else if (_player_chamber >= 0 && spore_front_chamber >= 0 &&
			_player_chamber > spore_front_chamber + _catch_up_threshold) {
		// Catch-up: speed up spores and accumulate handicap.
		effective_speed = speed * _catch_up_speed_multiplier;
		_spore_handicap += speed * (_catch_up_speed_multiplier - 1.0f) * p_delta;
	}

	_sweep += effective_speed * p_delta;

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
				if (c->chamber_id == 0) {
					speed *= _chamber_zero_speed_multiplier;
				}
			}
		}

		// Use effective depth (with noise) for sweep boundary.
		// This creates a wavy frontier instead of a straight line.
		float effective_depth = (float)c->depth + c->depth_noise * _depth_noise_amplitude;
		if (effective_depth > _sweep) {
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
	// Save old blocked state before recomputing.
	HashMap<Vector3i, bool> old_blocked;
	for (const auto &E : _cells) {
		old_blocked[E.key] = E.value.blocked_by_ward;
	}

	// Recompute blocked_by_ward for all cells using the spatial grid.
	if (_wards.is_empty()) {
		for (auto &E : _cells) {
			E.value.blocked_by_ward = false;
		}
	} else {
		for (auto &E : _cells) {
			bool blocked = false;
			Vector3 pos = E.value.world_pos;
			Vector3i center_key(
				int(Math::floor(pos.x / WARD_CELL_SIZE)),
				int(Math::floor(pos.y / WARD_CELL_SIZE)),
				int(Math::floor(pos.z / WARD_CELL_SIZE))
			);
			for (int dx = -1; dx <= 1 && !blocked; dx++) {
				for (int dy = -1; dy <= 1 && !blocked; dy++) {
					for (int dz = -1; dz <= 1 && !blocked; dz++) {
						Vector3i key = center_key + Vector3i(dx, dy, dz);
						const Vector<int32_t> *ward_ids = _ward_grid.getptr(key);
						if (!ward_ids) {
							continue;
						}
						for (int32_t wid : *ward_ids) {
							if (pos.distance_squared_to(_wards[wid].pos) < _wards[wid].radius * _wards[wid].radius) {
								blocked = true;
								break;
							}
						}
					}
				}
			}
			E.value.blocked_by_ward = blocked;
		}
	}

	// First pass: record which cells changed blocked status AND capture
	// their old depths BEFORE we reset them.  We need the minimum depth
	// among newly-blocked cells so we can also reset cells behind them
	// (which may have stale depths that let the sweep bypass the ward).
	int newly_blocked_count = 0;
	int newly_unblocked_count = 0;
	int min_blocked_old_depth = INT_MAX; // minimum depth among cells that became blocked

	for (auto &E : _cells) {
		bool was_blocked = old_blocked.has(E.key) ? old_blocked[E.key] : false;
		if (E.value.blocked_by_ward != was_blocked) {
			if (E.value.blocked_by_ward) {
				newly_blocked_count++;
				if (E.value.depth >= 0 && E.value.depth < min_blocked_old_depth) {
					min_blocked_old_depth = E.value.depth;
				}
			} else {
				newly_unblocked_count++;
			}
			// Reset depth for every cell whose blocked status changed.
			E.value.depth = -1;
		}
		// Belt-and-suspenders: all blocked cells must have no depth.
		if (E.value.blocked_by_ward) {
			E.value.depth = -1;
		}
	}

	// When wards activated (cells became blocked), any cell at a depth
	// ≥ the ward's minimum depth could now be UNREACHABLE or need a
	// longer path around the ward.  Reset them all so the BFS recomputes
	// correct depths — stale depths behind a ward are the #1 cause of
	// spores "walking through" an active ward.
	int behind_reset_count = 0;
	if (min_blocked_old_depth < INT_MAX) {
		for (auto &E : _cells) {
			if (!E.value.blocked_by_ward && E.value.depth >= min_blocked_old_depth) {
				E.value.depth = -1;
				behind_reset_count++;
			}
		}
	}

	if (newly_blocked_count > 0 || newly_unblocked_count > 0) {
		print_line(vformat("SporeManager::on_wards_changed  blocked:+%d  unblocked:+%d  min_depth=%d  behind_reset=%d",
			newly_blocked_count, newly_unblocked_count,
			min_blocked_old_depth < INT_MAX ? min_blocked_old_depth : -1,
			behind_reset_count));
	}

	// Rebuild the frontier set: find all unblocked cells with valid
	// depth that border unblocked depth=-1 cells.
	_frontier_set.clear();
	float max_frontier_depth = 0.0f;
	for (const auto &E : _cells) {
		if (E.value.blocked_by_ward || E.value.depth < 0) {
			continue;
		}
		for (const Vector3i &n : _bfs_neighbors) {
			Vector3i nk = E.key + n;
			const Cell *nc = _cells.getptr(nk);
			if (nc && !nc->blocked_by_ward && nc->depth < 0) {
				_frontier_set.insert(E.key);
				if ((float)E.value.depth > max_frontier_depth) {
					max_frontier_depth = (float)E.value.depth;
				}
				break;
			}
		}
	}

	// Save a snapshot of unvisited cells before the BFS.  When ONLY
	// unblocking (no new blocks), we offset their post-BFS depths so
	// the unblocked region resumes at the current sweep line instead
	// of its natural shallow depth.  This prevents both burst and
	// pause: the unblocked cells become the next to spawn, right after
	// the current front.
	HashSet<Vector3i> pre_bfs_unvisited;
	bool do_offset = (newly_unblocked_count > 0 && newly_blocked_count == 0);
	if (do_offset) {
		for (const auto &E : _cells) {
			if (E.value.depth < 0 && !E.value.blocked_by_ward) {
				pre_bfs_unvisited.insert(E.key);
			}
		}
	}

	// When a ward is removed (unblocking), the cells that were cut off
	// may extend far past max_frontier_depth.  Don't ask the BFS to
	// reach all the way to _sweep — the depth offset below handles the
	// gap.  Only extend enough to reconnect the graph and seed the
	// newly-unblocked region.
	float target;
	if (do_offset) {
		target = MAX(max_frontier_depth + BFS_LOOKAHEAD, _sweep + BFS_LOOKAHEAD);
	} else {
		target = MAX(max_frontier_depth + BFS_LOOKAHEAD, _sweep + BFS_LOOKAHEAD * 2);
	}
	print_line(vformat("SporeManager::on_wards_changed  BFS target=%.1f  frontier=%d  max_fdepth=%.1f  sweep=%.1f",
		target, _frontier_set.size(), max_frontier_depth, _sweep));
	_run_bfs_incremental(target);

	// Offset newly-assigned depths so the unblocked region starts at
	// the current sweep line.  Only offset cells whose assigned depth
	// is below the old max_frontier_depth — those are in the shallow
	// unblocked region.  Cells at/above max_frontier_depth are the
	// normal frontier extension and must keep their natural depths.
	if (do_offset && !pre_bfs_unvisited.is_empty()) {
		int min_new_depth = INT_MAX;
		for (const Vector3i &key : pre_bfs_unvisited) {
			const Cell *c = _cells.getptr(key);
			if (c && c->depth >= 0) {
				// Only consider cells that landed in the shallow region.
				if ((float)c->depth < max_frontier_depth && c->depth < min_new_depth) {
					min_new_depth = c->depth;
				}
			}
		}
		if (min_new_depth < INT_MAX) {
			int offset = (int)_sweep - min_new_depth;
			if (offset > 0) {
				for (const Vector3i &key : pre_bfs_unvisited) {
					Cell *c = _cells.getptr(key);
					if (c && c->depth >= 0 && (float)c->depth < max_frontier_depth) {
						c->depth += offset;
					}
				}
				print_line(vformat("SporeManager::on_wards_changed  depth offset +%d applied (min_was=%d sweep=%.1f)",
					offset, min_new_depth, _sweep));
			}
		}
	}
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

float SporeManager::get_sweep_depth() const {
	return _sweep;
}

void SporeManager::set_sweep(float p_sweep) {
	_sweep = p_sweep;
}

void SporeManager::set_client_mode(bool p_enabled) {
	_client_mode = p_enabled;
}

bool SporeManager::is_client_mode() const {
	return _client_mode;
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

void SporeManager::notify_cells_added() {
	_cells_added = true;
}

void SporeManager::ensure_depths_computed() {
	// Run full BFS to assign depth to every reachable cell.
	// Does NOT touch _sweep or _sweep_idx — only sets _sweep_dirty
	// so that advance_sweeps() rebuilds the sorted list on next call.
	// The rebuild correctly skips already-spawned cells, so the sweep
	// resumes exactly where it left off.
	_run_bfs_incremental(10000.0f);
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
	BIND_ENUM_CONSTANT(STATE_MATURE);
	BIND_ENUM_CONSTANT(STATE_SHRINKING);

	BIND_ENUM_CONSTANT(PROFILE_NORMAL);
	BIND_ENUM_CONSTANT(PROFILE_STRAIN);

	// Lifecycle
	ClassDB::bind_method(D_METHOD("add_spore", "position", "profile", "chamber_id", "spawn_depth"), &SporeManager::add_spore, DEFVAL(PROFILE_NORMAL), DEFVAL(-1), DEFVAL(-1.0f));
	ClassDB::bind_method(D_METHOD("add_spore_with_time", "position", "profile", "chamber_id", "spawn_depth", "spawn_time"), &SporeManager::add_spore_with_time);
	ClassDB::bind_method(D_METHOD("remove_spore", "id"), &SporeManager::remove_spore);
	ClassDB::bind_method(D_METHOD("remove_spores_in_chamber", "chamber_id", "shrink_first"), &SporeManager::remove_spores_in_chamber, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("shrink_spore", "id"), &SporeManager::shrink_spore);
	ClassDB::bind_method(D_METHOD("shrink_spores_in_chamber", "chamber_id"), &SporeManager::shrink_spores_in_chamber);
	ClassDB::bind_method(D_METHOD("set_spore_state", "id", "state"), &SporeManager::set_spore_state);
	ClassDB::bind_method(D_METHOD("get_spore_state", "id"), &SporeManager::get_spore_state);
	ClassDB::bind_method(D_METHOD("set_spore_profile", "id", "profile"), &SporeManager::set_spore_profile);
	ClassDB::bind_method(D_METHOD("get_spore_profile", "id"), &SporeManager::get_spore_profile);
	ClassDB::bind_method(D_METHOD("set_start_delay", "delay"), &SporeManager::set_start_delay);
	ClassDB::bind_method(D_METHOD("get_start_delay"), &SporeManager::get_start_delay);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "start_delay"), "set_start_delay", "get_start_delay");

	// Update
	ClassDB::bind_method(D_METHOD("update", "delta", "total_time"), &SporeManager::update);

	// Client mode (multiplayer)
	ClassDB::bind_method(D_METHOD("set_client_mode", "enabled"), &SporeManager::set_client_mode);
	ClassDB::bind_method(D_METHOD("is_client_mode"), &SporeManager::is_client_mode);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "client_mode"), "set_client_mode", "is_client_mode");

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

	// Depth-based lifecycle acceleration config
	ClassDB::bind_method(D_METHOD("set_depth_lifecycle_enabled", "enabled"), &SporeManager::set_depth_lifecycle_enabled);
	ClassDB::bind_method(D_METHOD("is_depth_lifecycle_enabled"), &SporeManager::is_depth_lifecycle_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "depth_lifecycle_enabled"), "set_depth_lifecycle_enabled", "is_depth_lifecycle_enabled");
	ClassDB::bind_method(D_METHOD("set_depth_lifecycle_mid_threshold", "threshold"), &SporeManager::set_depth_lifecycle_mid_threshold);
	ClassDB::bind_method(D_METHOD("get_depth_lifecycle_mid_threshold"), &SporeManager::get_depth_lifecycle_mid_threshold);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth_lifecycle_mid_threshold"), "set_depth_lifecycle_mid_threshold", "get_depth_lifecycle_mid_threshold");
	ClassDB::bind_method(D_METHOD("set_depth_lifecycle_full_threshold", "threshold"), &SporeManager::set_depth_lifecycle_full_threshold);
	ClassDB::bind_method(D_METHOD("get_depth_lifecycle_full_threshold"), &SporeManager::get_depth_lifecycle_full_threshold);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth_lifecycle_full_threshold"), "set_depth_lifecycle_full_threshold", "get_depth_lifecycle_full_threshold");
	ClassDB::bind_method(D_METHOD("set_depth_lifecycle_mid_multiplier", "multiplier"), &SporeManager::set_depth_lifecycle_mid_multiplier);
	ClassDB::bind_method(D_METHOD("get_depth_lifecycle_mid_multiplier"), &SporeManager::get_depth_lifecycle_mid_multiplier);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth_lifecycle_mid_multiplier"), "set_depth_lifecycle_mid_multiplier", "get_depth_lifecycle_mid_multiplier");

	// Mature phase & overlap cleanup config
	ClassDB::bind_method(D_METHOD("set_mature_phase_enabled", "enabled"), &SporeManager::set_mature_phase_enabled);
	ClassDB::bind_method(D_METHOD("is_mature_phase_enabled"), &SporeManager::is_mature_phase_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mature_phase_enabled"), "set_mature_phase_enabled", "is_mature_phase_enabled");

	ClassDB::bind_method(D_METHOD("set_overlap_cleanup_enabled", "enabled"), &SporeManager::set_overlap_cleanup_enabled);
	ClassDB::bind_method(D_METHOD("is_overlap_cleanup_enabled"), &SporeManager::is_overlap_cleanup_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "overlap_cleanup_enabled"), "set_overlap_cleanup_enabled", "is_overlap_cleanup_enabled");

	ClassDB::bind_method(D_METHOD("set_overlap_shrink_fraction", "fraction"), &SporeManager::set_overlap_shrink_fraction);
	ClassDB::bind_method(D_METHOD("get_overlap_shrink_fraction"), &SporeManager::get_overlap_shrink_fraction);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "overlap_shrink_fraction"), "set_overlap_shrink_fraction", "get_overlap_shrink_fraction");

	ClassDB::bind_method(D_METHOD("set_overlap_shrink_duration", "duration"), &SporeManager::set_overlap_shrink_duration);
	ClassDB::bind_method(D_METHOD("get_overlap_shrink_duration"), &SporeManager::get_overlap_shrink_duration);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "overlap_shrink_duration"), "set_overlap_shrink_duration", "get_overlap_shrink_duration");

	ClassDB::bind_method(D_METHOD("set_overlap_radius", "radius"), &SporeManager::set_overlap_radius);
	ClassDB::bind_method(D_METHOD("get_overlap_radius"), &SporeManager::get_overlap_radius);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "overlap_radius"), "set_overlap_radius", "get_overlap_radius");

	ClassDB::bind_method(D_METHOD("set_overlap_interval", "interval"), &SporeManager::set_overlap_interval);
	ClassDB::bind_method(D_METHOD("get_overlap_interval"), &SporeManager::get_overlap_interval);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "overlap_interval"), "set_overlap_interval", "get_overlap_interval");

	ClassDB::bind_method(D_METHOD("set_overlap_min_count", "count"), &SporeManager::set_overlap_min_count);
	ClassDB::bind_method(D_METHOD("get_overlap_min_count"), &SporeManager::get_overlap_min_count);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "overlap_min_count"), "set_overlap_min_count", "get_overlap_min_count");

	// Depth noise (frontier waviness)
	ClassDB::bind_method(D_METHOD("set_depth_noise_amplitude", "amplitude"), &SporeManager::set_depth_noise_amplitude);
	ClassDB::bind_method(D_METHOD("get_depth_noise_amplitude"), &SporeManager::get_depth_noise_amplitude);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth_noise_amplitude"), "set_depth_noise_amplitude", "get_depth_noise_amplitude");

	ClassDB::bind_method(D_METHOD("set_depth_noise_frequency", "frequency"), &SporeManager::set_depth_noise_frequency);
	ClassDB::bind_method(D_METHOD("get_depth_noise_frequency"), &SporeManager::get_depth_noise_frequency);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth_noise_frequency"), "set_depth_noise_frequency", "get_depth_noise_frequency");

	// Spore handicap (rubber-banding)
	ClassDB::bind_method(D_METHOD("set_player_chamber", "chamber"), &SporeManager::set_player_chamber);
	ClassDB::bind_method(D_METHOD("get_spore_handicap"), &SporeManager::get_spore_handicap);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spore_handicap"), "", "get_spore_handicap");

	ClassDB::bind_method(D_METHOD("set_catch_up_threshold", "threshold"), &SporeManager::set_catch_up_threshold);
	ClassDB::bind_method(D_METHOD("get_catch_up_threshold"), &SporeManager::get_catch_up_threshold);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "catch_up_threshold"), "set_catch_up_threshold", "get_catch_up_threshold");

	ClassDB::bind_method(D_METHOD("set_catch_up_speed_multiplier", "multiplier"), &SporeManager::set_catch_up_speed_multiplier);
	ClassDB::bind_method(D_METHOD("get_catch_up_speed_multiplier"), &SporeManager::get_catch_up_speed_multiplier);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "catch_up_speed_multiplier"), "set_catch_up_speed_multiplier", "get_catch_up_speed_multiplier");

	ClassDB::bind_method(D_METHOD("set_payback_speed_multiplier", "multiplier"), &SporeManager::set_payback_speed_multiplier);
	ClassDB::bind_method(D_METHOD("get_payback_speed_multiplier"), &SporeManager::get_payback_speed_multiplier);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "payback_speed_multiplier"), "set_payback_speed_multiplier", "get_payback_speed_multiplier");

	// Chamber 0 speed boost
	ClassDB::bind_method(D_METHOD("set_chamber_zero_speed_multiplier", "multiplier"), &SporeManager::set_chamber_zero_speed_multiplier);
	ClassDB::bind_method(D_METHOD("get_chamber_zero_speed_multiplier"), &SporeManager::get_chamber_zero_speed_multiplier);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chamber_zero_speed_multiplier"), "set_chamber_zero_speed_multiplier", "get_chamber_zero_speed_multiplier");

	// Per-chamber
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
	ClassDB::bind_method(D_METHOD("get_sweep_depth"), &SporeManager::get_sweep_depth);
	ClassDB::bind_method(D_METHOD("set_sweep", "sweep"), &SporeManager::set_sweep);
	ClassDB::bind_method(D_METHOD("set_spore_res", "res"), &SporeManager::set_spore_res);
	ClassDB::bind_method(D_METHOD("get_spore_res"), &SporeManager::get_spore_res);
	ClassDB::bind_method(D_METHOD("set_start_cell", "grid_key"), &SporeManager::set_start_cell);
	ClassDB::bind_method(D_METHOD("notify_cells_added"), &SporeManager::notify_cells_added);
	ClassDB::bind_method(D_METHOD("ensure_depths_computed"), &SporeManager::ensure_depths_computed);
	ClassDB::bind_method(D_METHOD("get_bfs_neighbor_count"), &SporeManager::get_bfs_neighbor_count);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spore_res"), "set_spore_res", "get_spore_res");
}
