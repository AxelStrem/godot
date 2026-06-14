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
#include "core/variant/typed_array.h"
#include "core/object/class_db.h"

// ---------------------------------------------------------------------------
// Growth parameters (mirrors GDScript constants)
// ---------------------------------------------------------------------------

static constexpr float MIN_RADIUS = 0.25f;
static constexpr float MAX_RADIUS_NORMAL = 50.0f;
static constexpr float MAX_RADIUS_STRAIN = 2.0f;

// Phase timing (seconds since spawn).
static constexpr float PHASE1_DURATION = 0.3f;   // snap to visible
static constexpr float PHASE2_DURATION = 14.7f;  // slow pulsing growth (0.5→2.0)
static constexpr float PHASE3_DURATION = 15.0f;  // accelerating growth (2.0→cap)

static constexpr float PULSE_FREQ = 3.0f;        // pulse oscillations per second
static constexpr float PHASE2_PULSE_AMP = 0.1f;  // pulse amplitude during phase 2
static constexpr float PHASE3_PULSE_AMP_INITIAL = 0.15f; // pulse amp at start of phase 3

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
		_seed_offsets.push_back(0.0f);
		_states.push_back(STATE_DEAD);
		_profiles.push_back(PROFILE_NORMAL);
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
	float cap = (p_profile == PROFILE_STRAIN) ? MAX_RADIUS_STRAIN : MAX_RADIUS_NORMAL;
	float base;
	float pulse_amp;

	if (p_elapsed < PHASE1_DURATION) {
		// Phase 1: fast snap to visible.
		float t = p_elapsed / PHASE1_DURATION;
		base = t * 0.5f;
		pulse_amp = 0.0f;
	} else if (p_elapsed < PHASE1_DURATION + PHASE2_DURATION) {
		// Phase 2: slow pulsing growth 0.5 → 2.0.
		float t = (p_elapsed - PHASE1_DURATION) / PHASE2_DURATION;
		base = 0.5f + t * 1.5f;
		pulse_amp = PHASE2_PULSE_AMP;
	} else if (p_elapsed < PHASE1_DURATION + PHASE2_DURATION + PHASE3_DURATION) {
		// Phase 3: accelerating growth 2.0 → cap.
		float phase3_elapsed = p_elapsed - PHASE1_DURATION - PHASE2_DURATION;
		float t = phase3_elapsed / PHASE3_DURATION;
		float t2 = t * t; // ease-in quadratic
		base = 2.0f + t2 * (cap - 2.0f);
		pulse_amp = PHASE3_PULSE_AMP_INITIAL * (1.0f - t); // pulse fades out
	} else {
		// Phase 4: stable at cap.
		base = cap;
		pulse_amp = 0.0f;
	}

	float pulse = Math::sin(p_elapsed * PULSE_FREQ + p_seed_offset) * pulse_amp;
	return MAX(base + pulse, 0.0001f);
}

// ---------------------------------------------------------------------------
// Public API — Spore lifecycle
// ---------------------------------------------------------------------------

int32_t SporeManager::add_spore(const Vector3 &p_pos, int p_profile) {
	int32_t id = _allocate_id();
	_positions.set(id, p_pos);
	_spawn_times.set(id, -1.0f); // Will be set by the first update() call.
	_seed_offsets.set(id, Math::randf() * 6.2831853f);
	_states.set(id, STATE_START_DELAY);
	_profiles.set(id, (p_profile == PROFILE_STRAIN) ? PROFILE_STRAIN : PROFILE_NORMAL);
	_alive.set(id, true);

	// Insert into spatial grid with initial tiny radius.
	_grid.insert(id, p_pos, 0.0001f);

	return id;
}

void SporeManager::remove_spore(int32_t p_id) {
	if (!_alive[p_id]) {
		return;
	}
	_grid.remove(p_id);
	_free_id(p_id);
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
		float old_radius = _radii[id];
		float new_radius = _compute_radius(elapsed, _profiles[id], _seed_offsets[id]);
		_radii.set(id, new_radius);

		// Update grid if the level changed.
		int old_level = SporeGrid::_level_for_radius(old_radius);
		int new_level = SporeGrid::_level_for_radius(new_radius);
		if (old_level != new_level) {
			_grid.migrate(id, _positions[id], old_radius, _positions[id], new_radius);
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
// Spatial queries
// ---------------------------------------------------------------------------

Vector<int32_t> SporeManager::query_nearby(const Vector3 &p_pos) const {
	Vector<int32_t> ids;
	_grid.query_nearby(p_pos, ids);

	Vector<int32_t> result;
	for (int32_t id : ids) {
		if (_alive[id]) {
			result.push_back(id);
		}
	}
	return result;
}

Vector<int32_t> SporeManager::query_sphere(const Vector3 &p_center, float p_radius) const {
	Vector<int32_t> ids;
	_grid.query_sphere(p_center, p_radius, ids);

	Vector<int32_t> result;
	for (int32_t id : ids) {
		if (_alive[id]) {
			result.push_back(id);
		}
	}
	return result;
}

Vector<int32_t> SporeManager::query_spores_in_range(const Vector3 &p_pos, float p_radius) const {
	// Use the grid to get candidates, then filter by actual distance.
	Vector<int32_t> candidates;
	_grid.query_nearby(p_pos, candidates);

	Vector<int32_t> result;
	float r2 = p_radius * p_radius;
	for (int32_t id : candidates) {
		if (!_alive[id]) {
			continue;
		}
		float dist2 = p_pos.distance_squared_to(_positions[id]);
		float total_r = p_radius + _radii[id];
		if (dist2 < total_r * total_r) {
			result.push_back(id);
		}
	}
	return result;
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
	ClassDB::bind_method(D_METHOD("add_spore", "position", "profile"), &SporeManager::add_spore, DEFVAL(PROFILE_NORMAL));
	ClassDB::bind_method(D_METHOD("remove_spore", "id"), &SporeManager::remove_spore);
	ClassDB::bind_method(D_METHOD("set_spore_state", "id", "state"), &SporeManager::set_spore_state);
	ClassDB::bind_method(D_METHOD("get_spore_state", "id"), &SporeManager::get_spore_state);
	ClassDB::bind_method(D_METHOD("set_spore_profile", "id", "profile"), &SporeManager::set_spore_profile);
	ClassDB::bind_method(D_METHOD("get_spore_profile", "id"), &SporeManager::get_spore_profile);

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

	// Wards
	ClassDB::bind_method(D_METHOD("set_wards", "positions", "radii"), &SporeManager::set_wards);
	ClassDB::bind_method(D_METHOD("is_spore_warded", "id"), &SporeManager::is_spore_warded);
}
