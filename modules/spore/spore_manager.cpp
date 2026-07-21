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

// Minimum radius when a spore is force-limited by a ward.
// Mirrors SporeConfig.WARD_MIN_SPORE_RADIUS in GDScript.
static constexpr float MIN_FORCE_LIMITED_RADIUS = 0.1f;

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
		_prune_elapsed.push_back(-1.0f);
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

	float cap = (p_profile == PROFILE_STRAIN) ? _max_radius_strain : _max_radius_normal;
	float base;
	float pulse_amp;

	if (t_elapsed < _phase1_duration) {
		// Phase 1: fast burst into view.
		float t = t_elapsed / _phase1_duration;
		base = t * 0.5f;
		pulse_amp = 0.0f;
	} else if (t_elapsed < _phase1_duration + _phase2_duration) {
		// Phase 2: slow pulsing growth 0.5 → 2.0.
		float t = (t_elapsed - _phase1_duration) / _phase2_duration;
		base = 0.5f + t * 1.5f;
		pulse_amp = PHASE2_PULSE_AMP;
	} else if (t_elapsed < _phase1_duration + _phase2_duration + _phase3_duration) {
		// Phase 3: accelerating growth 2.0 → cap.
		float phase3_elapsed = t_elapsed - _phase1_duration - _phase2_duration;
		float t = phase3_elapsed / _phase3_duration;
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

	_assign_prune_deadline(id, p_pos, p_spawn_depth);

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

	_assign_prune_deadline(id, p_pos, p_spawn_depth);

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

void SporeManager::set_max_radius_normal(float p_radius) {
	_max_radius_normal = MAX(p_radius, 0.5f);
}

float SporeManager::get_max_radius_normal() const {
	return _max_radius_normal;
}

void SporeManager::set_max_radius_strain(float p_radius) {
	_max_radius_strain = MAX(p_radius, 0.1f);
}

float SporeManager::get_max_radius_strain() const {
	return _max_radius_strain;
}

void SporeManager::set_phase1_duration(float p_duration) {
	_phase1_duration = MAX(p_duration, 0.01f);
}

float SporeManager::get_phase1_duration() const {
	return _phase1_duration;
}

void SporeManager::set_phase2_duration(float p_duration) {
	_phase2_duration = MAX(p_duration, 0.01f);
}

float SporeManager::get_phase2_duration() const {
	return _phase2_duration;
}

void SporeManager::set_phase3_duration(float p_duration) {
	_phase3_duration = MAX(p_duration, 0.01f);
}

float SporeManager::get_phase3_duration() const {
	return _phase3_duration;
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
				// Quadratic blend: ramps slowly at first, then hard.
				// At gap 60% through the mid→full window, accel is already
				// 36% of max, ensuring spores behind the frontier age out fast.
				float accel_blend = blend * blend;
				float accel = 1.0f + accel_blend * MAX(_depth_lifecycle_mid_multiplier - 1.0f, 0.0f);
				effective_elapsed = elapsed * accel;
			}
		}

		// ---- Prune-based lifecycle ----
		// If the spore has an active prune deadline and it has passed,
		// transition to shrinking.  Shrinking handles radius reduction
		// and eventual free deterministically on all peers.
		if (_prune_enabled && _prune_elapsed[id] > 0.0f && effective_elapsed >= _prune_elapsed[id] && _states[id] != STATE_SHRINKING) {
			shrink_spore(id);
			continue;
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
				if (growth_elapsed >= _phase1_duration + _phase2_duration + _phase3_duration) {
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

PackedVector3Array SporeManager::get_all_spore_positions() const {
	PackedVector3Array result;
	for (int32_t id : _alive_ids) {
		result.push_back(_positions[id]);
	}
	return result;
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
	// Save old wards for diffing.
	Vector<Ward> old_wards = _wards;

	// Rebuild ward list and spatial grid from the new data.
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

	// Diff: match old wards to new wards by position proximity.
	// Unmatched old wards were removed → on_ward_deactivated.
	// Unmatched new wards are fresh placements → on_ward_activated.
	// Matched wards that moved or changed radius → deactivate old + activate new.

	const float MATCH_TOLERANCE = 2.0f; // max distance to consider "same ward"
	HashSet<int> matched_old;
	HashSet<int> matched_new;

	for (int oi = 0; oi < old_wards.size(); oi++) {
		const Ward &ow = old_wards[oi];
		int best_ni = -1;
		float best_dist2 = MATCH_TOLERANCE * MATCH_TOLERANCE;
		for (int ni = 0; ni < _wards.size(); ni++) {
			if (matched_new.has(ni)) {
				continue;
			}
			float d2 = ow.pos.distance_squared_to(_wards[ni].pos);
			if (d2 < best_dist2) {
				best_dist2 = d2;
				best_ni = ni;
			}
		}
		if (best_ni >= 0) {
			matched_old.insert(oi);
			matched_new.insert(best_ni);
			// If the ward moved or radius changed, treat as deactivate + activate.
			const Ward &nw = _wards[best_ni];
			if (best_dist2 > 0.01f || Math::abs(ow.radius - nw.radius) > 0.1f) {
				on_ward_deactivated(ow.pos, ow.radius);
				on_ward_activated(nw.pos, nw.radius);
			}
		} else {
			// Old ward no longer exists.
			on_ward_deactivated(ow.pos, ow.radius);
		}
	}

	for (int ni = 0; ni < _wards.size(); ni++) {
		if (!matched_new.has(ni)) {
			// New ward that wasn't matched to any old ward.
			on_ward_activated(_wards[ni].pos, _wards[ni].radius);
		}
	}
}

Vector<Vector3i> SporeManager::_query_cells_in_ward_sphere(const Vector3 &p_center, float p_radius) const {
	// Enumerate all integer grid positions within the sphere.
	// Cells live on a regular grid with spacing _spore_res (default 1.0),
	// so we iterate grid keys in [center - radius, center + radius] and
	// do cheap _cells.getptr lookups.  Most grid slots are empty, so the
	// real cost is O(ward_volume / spore_res³) lookups, not O(all cells).
	Vector<Vector3i> result;
	float r2 = p_radius * p_radius;
	float inv_res = 1.0f / _spore_res;
	int r_cells = (int)Math::ceil(p_radius * inv_res);
	Vector3i center_cell(
		(int)Math::round(p_center.x * inv_res),
		(int)Math::round(p_center.y * inv_res),
		(int)Math::round(p_center.z * inv_res)
	);
	for (int dx = -r_cells; dx <= r_cells; dx++) {
		for (int dy = -r_cells; dy <= r_cells; dy++) {
			for (int dz = -r_cells; dz <= r_cells; dz++) {
				Vector3i key = center_cell + Vector3i(dx, dy, dz);
				const Cell *c = _cells.getptr(key);
				if (!c) {
					continue;
				}
				// Verify the cell's world position is actually within the sphere.
				if (c->world_pos.distance_squared_to(p_center) <= r2) {
					result.push_back(key);
				}
			}
		}
	}
	return result;
}

bool SporeManager::_is_position_warded(const Vector3 &p_pos) const {
	if (_wards.is_empty()) {
		return false;
	}
	Vector3i center_key(
		int(Math::floor(p_pos.x / WARD_CELL_SIZE)),
		int(Math::floor(p_pos.y / WARD_CELL_SIZE)),
		int(Math::floor(p_pos.z / WARD_CELL_SIZE))
	);
	for (int dx = -1; dx <= 1; dx++) {
		for (int dy = -1; dy <= 1; dy++) {
			for (int dz = -1; dz <= 1; dz++) {
				Vector3i key = center_key + Vector3i(dx, dy, dz);
				const Vector<int32_t> *ward_ids = _ward_grid.getptr(key);
				if (!ward_ids) {
					continue;
				}
				for (int32_t wid : *ward_ids) {
					if (p_pos.distance_squared_to(_wards[wid].pos) < _wards[wid].radius * _wards[wid].radius) {
						return true;
					}
				}
			}
		}
	}
	return false;
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

// ---- Prune-based lifecycle config ----

void SporeManager::set_prune_enabled(bool p_enabled) {
	_prune_enabled = p_enabled;
}

bool SporeManager::is_prune_enabled() const {
	return _prune_enabled;
}

void SporeManager::set_prune_fraction_immortal(float p_fraction) {
	_prune_fraction_immortal = CLAMP(p_fraction, 0.0f, 1.0f);
}

float SporeManager::get_prune_fraction_immortal() const {
	return _prune_fraction_immortal;
}

void SporeManager::set_prune_mean_elapsed(float p_mean) {
	_prune_mean_elapsed = MAX(p_mean, 1.0f);
}

float SporeManager::get_prune_mean_elapsed() const {
	return _prune_mean_elapsed;
}

void SporeManager::set_prune_min_elapsed(float p_min) {
	_prune_min_elapsed = MAX(p_min, 0.0f);
}

float SporeManager::get_prune_min_elapsed() const {
	return _prune_min_elapsed;
}

void SporeManager::_assign_prune_deadline(int32_t p_id, const Vector3 &p_pos, float p_spawn_depth) {
	if (!_prune_enabled) {
		_prune_elapsed.set(p_id, -1.0f);
		return;
	}
	// Deterministic pseudo-random from spawn position + depth so
	// host and client compute the same deadline for the same spore.
	float h = p_pos.x * 12.9898f + p_pos.y * 78.233f + p_pos.z * 37.719f + p_spawn_depth * 45.641f;
	h -= Math::floor(h);
	float r = h * 13.2436f + 7.1823f;
	r -= Math::floor(r);
	if (r < _prune_fraction_immortal) {
		_prune_elapsed.set(p_id, -1.0f);
		return;
	}
	// Second hash for the exponential distribution.
	h = p_pos.x * 93.121f + p_pos.y * 51.337f + p_pos.z * 17.419f + p_spawn_depth * 29.773f;
	h -= Math::floor(h);
	float r2 = h * 7.913f + 3.141f;
	r2 -= Math::floor(r2);
	// Exponential CDF inverse: -mean * ln(1 - U)
	float deadline = _prune_min_elapsed - _prune_mean_elapsed * Math::log(1.0f - r2);
	_prune_elapsed.set(p_id, MAX(deadline, _prune_min_elapsed));
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

PackedFloat32Array SporeManager::get_spore_ages_for_chamber(int p_chamber_id) const {
	PackedFloat32Array result;
	for (int32_t id : _alive_ids) {
		if (_chamber_ids[id] == p_chamber_id) {
			// age = total seconds since spawn (drives fragment shader color)
			float age = static_cast<float>(_last_total_time - static_cast<double>(_spawn_times[id]));
			result.push_back(age);
			// radius = current world radius (drives billboard scale)
			result.push_back(_radii[id]);
		}
	}
	return result;
}

PackedFloat32Array SporeManager::get_spore_buffer_for_chamber(int p_chamber_id) const {
	// Interleaved layout: 16 floats per instance.
	// [0..11] = 3x4 transform matrix (column-major, scale=radius)
	// [12..15] = color (age, radius, 0, 1) for the billboard shader.
	int count = 0;
	for (int32_t id : _alive_ids) {
		if (_chamber_ids[id] == p_chamber_id) {
			count++;
		}
	}

	PackedFloat32Array buffer;
	buffer.resize(count * 16);
	float *ptr = buffer.ptrw();
	int idx = 0;
	for (int32_t id : _alive_ids) {
		if (_chamber_ids[id] != p_chamber_id) {
			continue;
		}
		const Vector3 &pos = _positions[id];
		float r = _radii[id];
		float age = static_cast<float>(_last_total_time - static_cast<double>(_spawn_times[id]));
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
		// Color: (age, radius, 0, 1)
		ptr[idx + 12] = age;
		ptr[idx + 13] = r;
		ptr[idx + 14] = 0.0f;
		ptr[idx + 15] = 1.0f;
		idx += 16;
	}
	return buffer;
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
	c.blocked_by_ward = _is_position_warded(p_world_pos);
	c.dead = false;

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

	print_line(vformat("SporeManager::_run_bfs_incremental  START  target=%.1f  sweep=%.1f  bfs_computed_depth=%.1f  frontier_set=%d",
		p_target_depth, _sweep, _bfs_computed_depth, _frontier_set.size()));

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
	int frontier_seeded = 0;
	int frontier_skipped_dead = 0;
	int frontier_skipped_blocked = 0;
	int frontier_skipped_depth_neg = 0;
	int frontier_skipped_depth_oob = 0;
	int frontier_skipped_no_neighbor = 0;
	if (!_frontier_set.is_empty()) {
		for (const Vector3i &key : _frontier_set) {
			if (visited.has(key)) {
				continue;
			}
			const Cell *cell_ref = _cells.getptr(key);
			if (!cell_ref) {
				continue;
			}
			if (cell_ref->dead) {
				frontier_skipped_dead++;
				continue;
			}
			if (cell_ref->blocked_by_ward) {
				frontier_skipped_blocked++;
				continue;
			}
			if (cell_ref->depth < 0) {
				frontier_skipped_depth_neg++;
				continue;
			}
			if (cell_ref->depth > (int)p_target_depth) {
				frontier_skipped_depth_oob++;
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
				frontier_seeded++;
			} else {
				frontier_skipped_no_neighbor++;
			}
		}
	}

	print_line(vformat("SporeManager::_run_bfs_incremental  SEEDS  wave=%d  start=%d  entry=%d  frontier(seeded=%d skip:dead=%d blocked=%d neg_depth=%d oob=%d no_nbr=%d)",
		wave.size(),
		_has_start_cell ? 1 : 0,
		wave.size() - (_has_start_cell ? 1 : 0) - frontier_seeded,
		frontier_seeded,
		frontier_skipped_dead, frontier_skipped_blocked, frontier_skipped_depth_neg,
		frontier_skipped_depth_oob, frontier_skipped_no_neighbor));

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
				if (!c || c->dead || c->blocked_by_ward) {
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
	_bfs_global_max_depth = MAX(_bfs_global_max_depth, p_target_depth);
	_sweep_dirty = true;

	// Rebuild _frontier_set from the visited set (cells touched in
	// this BFS run only, NOT the whole cell graph).  Any visited cell
	// that still has an unvisited neighbour is a frontier candidate.
	_frontier_set.clear();
	for (const Vector3i &key : visited) {
		const Cell *cell_ref = _cells.getptr(key);
		if (!cell_ref || cell_ref->dead || cell_ref->blocked_by_ward || cell_ref->depth < 0) {
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

void SporeManager::_run_clean_bfs_from() {
	print_line(vformat("SporeManager::_run_clean_bfs_from  START  sweep=%.1f  spore_frontier=%d  bfs_computed_depth=%.1f",
		_sweep, _spore_frontier.size(), _bfs_computed_depth));

	// Reset all non-dead, non-spore-frontier cell depths to -1.
	// This discards stale BFS depths from before a ward was placed/removed
	// and ensures the re-flood finds the current shortest paths.
	int reset_count = 0;
	for (auto &E : _cells) {
		if (E.value.dead) {
			continue;
		}
		if (_spore_frontier.has(E.key)) {
			continue; // Anchor cells keep their existing depth.
		}
		if (E.value.depth >= 0) {
			E.value.depth = -1;
			reset_count++;
		}
	}

	// Rebuild _frontier_set exclusively from spore frontier cells.
	_init_bfs_neighbors();
	_frontier_set.clear();
	for (const Vector3i &key : _spore_frontier) {
		const Cell *c = _cells.getptr(key);
		if (!c || c->dead || c->blocked_by_ward || c->depth < 0) {
			continue;
		}
		for (const Vector3i &n : _bfs_neighbors) {
			Vector3i nk = key + n;
			const Cell *nc = _cells.getptr(nk);
			if (nc && !nc->dead && !nc->blocked_by_ward && nc->depth < 0) {
				_frontier_set.insert(key);
				break;
			}
		}
	}

	// The spore frontier anchors sit at or near _sweep.  Run BFS
	// far enough ahead to cover a generous lookahead window.
	_bfs_computed_depth = _sweep;
	float target = _sweep + BFS_LOOKAHEAD * 3.0f;
	_run_bfs_incremental(target);

	print_line(vformat("SporeManager::_run_clean_bfs_from  reset=%d  frontier=%d  target=%.1f",
		reset_count, _frontier_set.size(), target));
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
		if (E.value.dead) {
			continue;
		}
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
	// Reset all non-dead cells to unvisited.  Dead cells are immutable:
	// their spores have already spawned and their depth is final.
	for (auto &E : _cells) {
		if (!E.value.dead) {
			E.value.depth = -1;
		}
	}

	// Full re-initialization — discard stale spore frontier entries
	// and the global max depth (cell graph is rebuilt from scratch).
	_spore_frontier.clear();
	_bfs_global_max_depth = 0.0f;

	// Run full BFS to cover all cells in the fresh graph.
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
		print_line(vformat("SporeManager::advance_sweeps  BFS_EXTEND(cells_added)  target=%.1f", target));
		_run_bfs_incremental(target);
		_cells_added = false;
	} else if (_sweep + BFS_LOOKAHEAD * 0.5f > _bfs_computed_depth) {
		// Always extend far enough ahead of the sweep cursor so the
		// BFS actually reaches the frontier even if _bfs_computed_depth
		// was left stale by a ward change or other edge case.
		float ext_target = MAX(_bfs_computed_depth + BFS_LOOKAHEAD, _sweep + BFS_LOOKAHEAD * 2);
		print_line(vformat("SporeManager::advance_sweeps  BFS_EXTEND(sweep_near)  sweep=%.1f  computed=%.1f  target=%.1f",
			_sweep, _bfs_computed_depth, ext_target));
		_run_bfs_incremental(ext_target);
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

		// Skip dead cells (should not appear in _sorted_cells, but guard anyway).
		if (c->dead) {
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
	if (!c) {
		return;
	}
	if (c->spawned) {
		return; // Already spawned, nothing to do.
	}
	c->spawned = true;
	_update_spore_frontier_for_spawn(p_grid_key);
}

SporeManager::CellZone SporeManager::_classify_cell(const Vector3i &p_key) const {
	const Cell *c = _cells.getptr(p_key);
	if (!c) {
		return CellZone::DEAD; // Non-existent cells are irrelevant.
	}
	if (c->dead) {
		return CellZone::DEAD;
	}
	if (!c->spawned) {
		return CellZone::AHEAD;
	}
	// Spawned and not dead: check if it still borders unspawned territory.
	for (const Vector3i &n : _bfs_neighbors) {
		Vector3i nk = p_key + n;
		const Cell *nc = _cells.getptr(nk);
		if (nc && !nc->blocked_by_ward && !nc->spawned) {
			return CellZone::SPORE_FRONTIER;
		}
	}
	return CellZone::DEAD;
}

void SporeManager::_update_spore_frontier_for_spawn(const Vector3i &p_key) {
	// Classify the newly-spawned cell.
	CellZone zone = _classify_cell(p_key);
	Cell *c = _cells.getptr(p_key);
	if (!c) {
		return;
	}

	switch (zone) {
		case CellZone::SPORE_FRONTIER:
			_spore_frontier.insert(p_key);
			break;
		case CellZone::DEAD:
			c->dead = true;
			break;
		case CellZone::AHEAD:
			// Should not happen — we just set spawned=true.
			break;
	}

	// Re-classify neighbors that were in the spore frontier.
	// They may have lost their last unspawned neighbor (this cell).
	for (const Vector3i &n : _bfs_neighbors) {
		Vector3i nk = p_key + n;
		if (!_spore_frontier.has(nk)) {
			continue;
		}
		CellZone nz = _classify_cell(nk);
		if (nz == CellZone::DEAD) {
			_spore_frontier.erase(nk);
			Cell *nc = _cells.getptr(nk);
			if (nc) {
				nc->dead = true;
			}
		}
		// If still SPORE_FRONTIER, it stays in the set.
	}
}

void SporeManager::on_ward_activated(const Vector3 &p_center, float p_radius) {
	// Query only the cells inside this ward's sphere — O(ward_volume),
	// not O(all_cells).  Dead cells are implicitly excluded because
	// _query_cells_in_ward_sphere doesn't filter by zone; we classify below.
	Vector<Vector3i> affected = _query_cells_in_ward_sphere(p_center, p_radius);

	Vector<Vector3i> cells_to_block;
	int min_blocked_old_depth = INT_MAX;
	int spore_frontier_blocked = 0;
	int ahead_with_depth = 0;
	int ahead_no_depth = 0;

	for (const Vector3i &key : affected) {
		Cell *c = _cells.getptr(key);
		if (!c) {
			continue;
		}
		if (c->blocked_by_ward) {
			continue; // Already blocked, nothing to do.
		}

		CellZone zone = _classify_cell(key);
		if (zone == CellZone::DEAD) {
			continue; // Dead cells are immutable — ignore.
		}

		cells_to_block.push_back(key);

		if (zone == CellZone::SPORE_FRONTIER) {
			spore_frontier_blocked++;
		} else if (c->depth >= 0) {
			ahead_with_depth++;
		} else {
			ahead_no_depth++;
		}
		if (c->depth >= 0 && c->depth < min_blocked_old_depth) {
			min_blocked_old_depth = c->depth;
		}
	}

	if (cells_to_block.is_empty()) {
		print_line(vformat("SporeManager::on_ward_activated  pos=(%.1f,%.1f,%.1f) r=%.1f  NO_CELLS_TO_BLOCK",
			p_center.x, p_center.y, p_center.z, p_radius));
		return; // Ward fully inside dead zone — nothing to do.
	}

	// ---- Apply blocking ----
	_init_bfs_neighbors();
	int frontier_promoted_to_dead = 0;
	for (const Vector3i &key : cells_to_block) {
		Cell *c = _cells.getptr(key);
		if (!c) {
			continue;
		}
		c->blocked_by_ward = true;
		c->depth = -1;
		_spore_frontier.erase(key);
		_frontier_set.erase(key);

		// Re-classify spore frontier neighbours: the blocked cell may
		// have been their only unspawned, unblocked neighbour.  If so,
		// they transition from SPORE_FRONTIER → DEAD.  Keeping them in
		// _spore_frontier makes the rebuild loop below progressively
		// slower with each ward activation.
		for (const Vector3i &n : _bfs_neighbors) {
			Vector3i nk = key + n;
			if (!_spore_frontier.has(nk)) {
				continue;
			}
			CellZone nz = _classify_cell(nk);
			if (nz == CellZone::DEAD) {
				_spore_frontier.erase(nk);
				Cell *nc = _cells.getptr(nk);
				if (nc) {
					nc->dead = true;
				}
				frontier_promoted_to_dead++;
			}
		}
	}

	print_line(vformat("SporeManager::on_ward_activated  pos=(%.1f,%.1f,%.1f) r=%.1f  affected=%d  blocked=%d",
		p_center.x, p_center.y, p_center.z, p_radius, affected.size(), cells_to_block.size()));
	print_line(vformat("SporeManager::on_ward_activated  spore_frontier=%d  ahead_with_depth=%d  ahead_no_depth=%d  min_old_depth=%d  frontier_to_dead=%d",
		spore_frontier_blocked, ahead_with_depth, ahead_no_depth,
		min_blocked_old_depth < INT_MAX ? min_blocked_old_depth : -1, frontier_promoted_to_dead));

	// Per plan §5.1: if all blocked cells are beyond the BFS frontier
	// (none have a computed depth yet), no re-BFS is needed — the lazy
	// BFS extension will naturally skip them when it reaches them.
	// Set _sweep_dirty so _build_sweep_list excludes blocked cells.
	if (min_blocked_old_depth == INT_MAX) {
		print_line("SporeManager::on_ward_activated  path=EARLY_RETURN  (all blocked beyond BFS frontier, no re-BFS)");
		_sweep_dirty = true;
		return;
	}

	// ---- At least one blocked cell had a BFS depth ---
	// Cells at depth ≥ min_blocked_old_depth may have stale depths
	// computed through now-blocked cells.  Reset and re-flood BFS
	// from spore frontier anchors.
	print_line("SporeManager::on_ward_activated  path=RE_BFS  (cells with BFS depth blocked, re-flood from spore frontier)");

	// ---- Behind-reset ----
	int behind_reset_count = 0;
	for (auto &E : _cells) {
		if (E.value.dead || E.value.blocked_by_ward) {
			continue;
		}
		// Spore frontier cells are anchored — their depth is final.
		if (_spore_frontier.has(E.key)) {
			continue;
		}
		if (E.value.depth >= min_blocked_old_depth) {
			E.value.depth = -1;
			behind_reset_count++;
		}
	}
	print_line(vformat("SporeManager::on_ward_activated  behind_reset=%d", behind_reset_count));

	// Rebuild _frontier_set from spore frontier cells.  These are the
	// permanent BFS anchors — already spawned, depth is final, and they
	// should have unvisited neighbors after the behind-reset.
	_frontier_set.clear();
	_init_bfs_neighbors();
	for (const Vector3i &key : _spore_frontier) {
		const Cell *c = _cells.getptr(key);
		if (!c || c->blocked_by_ward || c->depth < 0) {
			continue;
		}
		// Verify the spore frontier cell still has unvisited neighbors.
		for (const Vector3i &n : _bfs_neighbors) {
			Vector3i nk = key + n;
			const Cell *nc = _cells.getptr(nk);
			if (nc && !nc->blocked_by_ward && nc->depth < 0) {
				_frontier_set.insert(key);
				break;
			}
		}
	}

	// Run the BFS from the rebuilt frontier, but only a reasonable
	// window ahead of the sweep.  The behind-reset cleared depths up
	// to _bfs_global_max_depth; we re-assign the near window here.
	// The lazy BFS extension in advance_sweeps() naturally fills in
	// cells beyond this window as the sweep cursor advances.
	float target = _sweep + BFS_LOOKAHEAD * 3.0f;
	print_line(vformat("SporeManager::on_ward_activated  re_bfs target=%.1f  spore_frontier=%d  bfs_frontier=%d",
		target, _spore_frontier.size(), _frontier_set.size()));
	_run_bfs_incremental(target);

	_sweep_dirty = true;
}

void SporeManager::on_ward_deactivated(const Vector3 &p_center, float p_radius) {
	// Query only the cells inside this ward's former sphere.
	Vector<Vector3i> affected = _query_cells_in_ward_sphere(p_center, p_radius);

	Vector<Vector3i> has_dead_neighbor_cells;
	Vector<Vector3i> has_visited_neighbor_cells;
	Vector<Vector3i> isolated_cells;

	_init_bfs_neighbors();
	for (const Vector3i &key : affected) {
		Cell *c = _cells.getptr(key);
		if (!c) {
			continue;
		}
		if (c->dead) {
			continue; // Dead cells stay dead; their blocked status is irrelevant.
		}
		if (!c->blocked_by_ward) {
			continue; // Not actually blocked — nothing to unblock.
		}

		// Classify by neighbor types.
		bool has_dead = false;
		bool has_visited = false;

		for (const Vector3i &n : _bfs_neighbors) {
			Vector3i nk = key + n;
			const Cell *nc = _cells.getptr(nk);
			if (!nc) {
				continue;
			}
			if (nc->dead) {
				has_dead = true;
				break; // Dead neighbor takes priority.
			}
			if (!nc->blocked_by_ward && nc->depth >= 0) {
				has_visited = true;
				// Don't break — dead check takes priority.
			}
		}

		if (has_dead) {
			has_dead_neighbor_cells.push_back(key);
		} else if (has_visited) {
			has_visited_neighbor_cells.push_back(key);
		} else {
			isolated_cells.push_back(key);
		}
	}

	if (has_dead_neighbor_cells.is_empty() &&
			has_visited_neighbor_cells.is_empty() &&
			isolated_cells.is_empty()) {
		return; // Nothing to do.
	}

	// ---- Unblock all affected cells ----
	auto unblock_cell = [this](const Vector3i &key) {
		Cell *c = _cells.getptr(key);
		if (c) {
			c->blocked_by_ward = false;
			c->depth = -1; // Will be recomputed.
		}
	};

	for (const Vector3i &key : has_dead_neighbor_cells) {
		unblock_cell(key);
	}
	for (const Vector3i &key : has_visited_neighbor_cells) {
		unblock_cell(key);
	}
	for (const Vector3i &key : isolated_cells) {
		unblock_cell(key);
	}

	print_line(vformat("SporeManager::on_ward_deactivated  dead_nbr=%d  visited_nbr=%d  isolated=%d",
		has_dead_neighbor_cells.size(), has_visited_neighbor_cells.size(), isolated_cells.size()));

	// ---- Case 1: Dead-neighbor cells → anchor at sweep depth, clean re-BFS ----
	if (!has_dead_neighbor_cells.is_empty()) {
		for (const Vector3i &key : has_dead_neighbor_cells) {
			Cell *c = _cells.getptr(key);
			if (c) {
				c->depth = (int)_sweep;
				_spore_frontier.insert(key);
			}
		}

		// Clean BFS: resets all AHEAD depths, rebuilds frontier from
		// spore frontier anchors, and re-floods from scratch.
		print_line(vformat("SporeManager::on_ward_deactivated  case=dead_nbr  spore_frontier=%d",
			_spore_frontier.size()));
		_run_clean_bfs_from();
		_sweep_dirty = true;
		return;
	}

	// ---- Case 2: Visited-neighbor cells → add to frontier, lazy BFS ----
	if (!has_visited_neighbor_cells.is_empty()) {
		for (const Vector3i &key : has_visited_neighbor_cells) {
			_frontier_set.insert(key);
		}

		float target = _bfs_computed_depth + BFS_LOOKAHEAD;
		print_line(vformat("SporeManager::on_ward_deactivated  case=visited_nbr  BFS target=%.1f  frontier=%d",
			target, _frontier_set.size()));
		_run_bfs_incremental(target);
		_sweep_dirty = true;
		return;
	}

	// ---- Case 3: Isolated cells only → no BFS needed ----
	// Cells were unblocked but have no neighbors with known depth.
	// They'll be discovered naturally by future lazy BFS extensions.
	// Nothing more to do.
	print_line(vformat("SporeManager::on_ward_deactivated  case=isolated  no BFS needed"));
}

void SporeManager::on_wards_changed() {
	// This method is now a no-op.  set_wards() automatically diffs
	// old vs new wards and calls on_ward_activated() /
	// on_ward_deactivated() for each change.  New cells added while
	// wards are active are checked in add_cell() via _is_position_warded().
	// Kept for backward compatibility — callers can safely remove it.
	print_line("SporeManager::on_wards_changed  (no-op, per-ward methods handle changes)");
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
	// Trigger a lazy BFS extension on the next advance_sweeps() call.
	// This replaces the old behaviour of running a full-graph BFS to
	// 10000/global-max — the lazy extension in advance_sweeps already
	// keeps the BFS window ahead of the sweep cursor, so a deep
	// recomputation is never needed.
	_cells_added = true;
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

	// Growth phase timing & radius caps
	ClassDB::bind_method(D_METHOD("set_max_radius_normal", "radius"), &SporeManager::set_max_radius_normal);
	ClassDB::bind_method(D_METHOD("get_max_radius_normal"), &SporeManager::get_max_radius_normal);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_radius_normal"), "set_max_radius_normal", "get_max_radius_normal");

	ClassDB::bind_method(D_METHOD("set_max_radius_strain", "radius"), &SporeManager::set_max_radius_strain);
	ClassDB::bind_method(D_METHOD("get_max_radius_strain"), &SporeManager::get_max_radius_strain);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_radius_strain"), "set_max_radius_strain", "get_max_radius_strain");

	ClassDB::bind_method(D_METHOD("set_phase1_duration", "duration"), &SporeManager::set_phase1_duration);
	ClassDB::bind_method(D_METHOD("get_phase1_duration"), &SporeManager::get_phase1_duration);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "phase1_duration"), "set_phase1_duration", "get_phase1_duration");

	ClassDB::bind_method(D_METHOD("set_phase2_duration", "duration"), &SporeManager::set_phase2_duration);
	ClassDB::bind_method(D_METHOD("get_phase2_duration"), &SporeManager::get_phase2_duration);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "phase2_duration"), "set_phase2_duration", "get_phase2_duration");

	ClassDB::bind_method(D_METHOD("set_phase3_duration", "duration"), &SporeManager::set_phase3_duration);
	ClassDB::bind_method(D_METHOD("get_phase3_duration"), &SporeManager::get_phase3_duration);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "phase3_duration"), "set_phase3_duration", "get_phase3_duration");

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
	ClassDB::bind_method(D_METHOD("get_all_spore_positions"), &SporeManager::get_all_spore_positions);

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

	// Prune-based lifecycle config
	ClassDB::bind_method(D_METHOD("set_prune_enabled", "enabled"), &SporeManager::set_prune_enabled);
	ClassDB::bind_method(D_METHOD("is_prune_enabled"), &SporeManager::is_prune_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "prune_enabled"), "set_prune_enabled", "is_prune_enabled");

	ClassDB::bind_method(D_METHOD("set_prune_fraction_immortal", "fraction"), &SporeManager::set_prune_fraction_immortal);
	ClassDB::bind_method(D_METHOD("get_prune_fraction_immortal"), &SporeManager::get_prune_fraction_immortal);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "prune_fraction_immortal"), "set_prune_fraction_immortal", "get_prune_fraction_immortal");

	ClassDB::bind_method(D_METHOD("set_prune_mean_elapsed", "mean"), &SporeManager::set_prune_mean_elapsed);
	ClassDB::bind_method(D_METHOD("get_prune_mean_elapsed"), &SporeManager::get_prune_mean_elapsed);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "prune_mean_elapsed"), "set_prune_mean_elapsed", "get_prune_mean_elapsed");

	ClassDB::bind_method(D_METHOD("set_prune_min_elapsed", "min_elapsed"), &SporeManager::set_prune_min_elapsed);
	ClassDB::bind_method(D_METHOD("get_prune_min_elapsed"), &SporeManager::get_prune_min_elapsed);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "prune_min_elapsed"), "set_prune_min_elapsed", "get_prune_min_elapsed");

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
	ClassDB::bind_method(D_METHOD("get_spore_ages_for_chamber", "chamber_id"), &SporeManager::get_spore_ages_for_chamber);
	ClassDB::bind_method(D_METHOD("get_spore_buffer_for_chamber", "chamber_id"), &SporeManager::get_spore_buffer_for_chamber);
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
