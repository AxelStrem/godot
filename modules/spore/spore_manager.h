/**************************************************************************/
/*  spore_manager.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/variant/type_info.h"

#include "spore_grid.h"

class SporeManager : public Object {
	GDCLASS(SporeManager, Object);

public:
	enum SporeState {
		STATE_DEAD = 0,
		STATE_START_DELAY,
		STATE_CONNECTING,
		STATE_ACTIVE,
		STATE_DYING,
	};

	enum Profile {
		PROFILE_NORMAL = 0,
		PROFILE_STRAIN = 1,
	};

private:
	// ---- Flat arrays (parallel arrays indexed by spore ID) ----
	// A spore is alive when `alive[id]` is true.
	// IDs are recycled via a free list.
	Vector<Vector3> _positions;
	Vector<float> _spawn_times;    // total_time when the spore was spawned
	Vector<float> _radii;          // current computed radius (updated each frame)
	Vector<float> _force_limits;  // per-spore max radius enforced by wards (0 = no limit)
	Vector<float> _seed_offsets;   // unique per-spore offset for pulse phase
	Vector<uint8_t> _states;       // SporeState
	Vector<uint8_t> _profiles;     // Profile
	Vector<int32_t> _chamber_ids;  // chamber number this spore belongs to (-1 = unowned)
	Vector<bool> _alive;
	Vector<int32_t> _free_list;    // recycled IDs
	Vector<int32_t> _alive_ids;    // compact list rebuilt each update() for fast iteration

	// Cached total_time from last update for on-demand radius computation.
	double _last_total_time = -1.0;

	// Configurable startup delay (seconds). Spore sits at radius 0 while
	// the parent tentacle grows toward it. Defaults to 1.5s.
	float _start_delay = 1.5f;
	float _force_limit_shrink_speed = 1.5f; // units/sec radius shrinks toward ward limit

	// ---- Spatial index ----
	SporeGrid _grid;

	// ---- Ward storage ----
	// Wards are simple (position, radius). Managed separately from spores.
	struct Ward {
		Vector3 pos;
		float radius;
	};
	Vector<Ward> _wards;
	// Coarse grid for wards (level 0 only, cell_size = 4).
	HashMap<Vector3i, Vector<int32_t>> _ward_grid;
	static constexpr float WARD_CELL_SIZE = 4.0f;

	// ---- Internal helpers ----
	int _allocate_id();
	void _free_id(int32_t p_id);
	float _compute_radius(float p_elapsed, int p_profile, float p_seed_offset) const;
	void _rebuild_ward_grid();

protected:
	static void _bind_methods();

public:
	SporeManager();
	~SporeManager();

	// ---- Spore lifecycle (called from GDScript) ----
	int32_t add_spore(const Vector3 &p_pos, int p_profile = PROFILE_NORMAL, int p_chamber_id = -1);
	void remove_spore(int32_t p_id);
	void remove_spores_in_chamber(int p_chamber_id);
	void set_spore_state(int32_t p_id, int p_state);
	int get_spore_state(int32_t p_id) const;
	void set_spore_profile(int32_t p_id, int p_profile);
	int get_spore_profile(int32_t p_id) const;
	void set_start_delay(float p_delay);
	float get_start_delay() const;

	// ---- Per-frame update ----
	void update(double p_delta, double p_total_time);

	// ---- Read current state (GDScript queries) ----
	float get_spore_radius(int32_t p_id) const;
	Vector3 get_spore_position(int32_t p_id) const;
	bool is_spore_alive(int32_t p_id) const;
	int get_spore_count() const;
	int get_active_spore_count() const;

	// ---- Spatial queries ----
	Vector<int32_t> query_nearby(const Vector3 &p_pos) const;
	Vector<int32_t> query_sphere(const Vector3 &p_center, float p_radius) const;
	bool is_any_spore_in_range(const Vector3 &p_center, float p_radius) const;
	bool is_any_dangerous_spore_in_range(const Vector3 &p_center, float p_radius) const;

	// ---- Ward management ----
	void set_wards(const TypedArray<Vector3> &p_positions, const TypedArray<float> &p_radii);
	bool is_spore_warded(int32_t p_id) const;

	// ---- Force-limit (ward suppression) ----
	void set_spore_force_limit(int32_t p_id, float p_limit);
	float get_spore_force_limit(int32_t p_id) const;
	void set_force_limit_shrink_speed(float p_speed);
	float get_force_limit_shrink_speed() const;

	// ---- For tentacle parent-finding (distance-filtered) ----
	// Returns spore IDs within `p_radius` of `p_pos`, performing actual
	// distance checks (unlike query_nearby which returns cell-neighbor candidates).
	Vector<int32_t> query_spores_in_range(const Vector3 &p_pos, float p_radius) const;

	// ---- Per-chamber queries (for MultiMesh rendering) ----
	// Returns a PackedFloat32Array where every 12 floats is a 3x4 transform
	// matrix (scale=radius, translate=position) for each spore in the chamber.
	// GDScript feeds this directly to MultiMeshInstance3D.buffer.
	PackedFloat32Array get_spore_transforms_for_chamber(int p_chamber_id) const;
	int get_spore_count_for_chamber(int p_chamber_id) const;
	int get_spore_chamber(int32_t p_id) const;
};

VARIANT_ENUM_CAST(SporeManager::SporeState);
VARIANT_ENUM_CAST(SporeManager::Profile);
