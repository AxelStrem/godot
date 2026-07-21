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
#include "core/templates/hash_set.h"
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
		STATE_MATURE,     // Reached full size; stays with subtle pulse (no lifetime death).
		STATE_SHRINKING,  // Shrinking to 0 before removal (visual polish for overlap cleanup).
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
	Vector<float> _spawn_depths;   // sweep depth of the cell that spawned this spore (-1 = unknown)
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

	// ---- Growth phase timing & radius caps (tunable from GDScript) ----
	float _max_radius_normal = 35.0f;
	float _max_radius_strain = 2.0f;
	float _phase1_duration = 0.6f;
	float _phase2_duration = 25.0f;
	float _phase3_duration = 20.0f;

	// Optional depth-based lifecycle acceleration: spores far behind the
	// current sweep frontier age faster so they compact/retire sooner.
	bool _depth_lifecycle_enabled = true;
	float _depth_lifecycle_mid_threshold = 15.0f;
	float _depth_lifecycle_full_threshold = 30.0f;
	float _depth_lifecycle_mid_multiplier = 30.0f;

	// ---- Mature phase & overlap cleanup (tunable from GDScript) ----
	// When enabled, spores survive past their old hard lifetime and
	// enter a mature phase at full size with subtle continuous pulse.
	// Overlap detection periodically culls redundant spores by shrinking
	// them first so the player sees them recede into the spore mass.
	bool _mature_phase_enabled = true;

	bool _overlap_cleanup_enabled = false;   // superseded by prune-based lifecycle
	float _overlap_shrink_fraction = 0.5f;   // fraction of overlapping pairs to cull (0–1)
	float _overlap_shrink_duration = 1.0f;   // seconds to shrink a spore before freeing it
	float _overlap_radius = 5.0f;            // centre-distance threshold for "overlapping"
	float _overlap_interval = 3.0f;          // seconds between overlap-detection passes
	int _overlap_min_count = 10;             // minimum spores in chamber before cleanup runs
	float _overlap_timer = 0.0f;             // accumulator for periodic checks

	// ---- Prune-based lifecycle (primary cleanup mechanism) ----
	// When enabled, each spore gets a deterministic prune deadline at spawn.
	// A fraction (_prune_fraction_immortal) are immortal (deadline = -1).
	// The remaining get an exponentially-distributed deadline offset from
	// _prune_min_elapsed.  When effective_elapsed crosses the deadline, the
	// spore transitions to SHRINKING and frees after _overlap_shrink_duration.
	// Deadlines are derived from spawn position + depth hash so host and
	// client reach the same conclusion deterministically.
	bool _prune_enabled = true;
	float _prune_fraction_immortal = 0.1f;   // fraction of spores that never prune (0–1)
	float _prune_mean_elapsed = 30.0f;       // mean of exponential deadline distribution (seconds)
	float _prune_min_elapsed = 3.0f;         // minimum effective elapsed before pruning can start
	Vector<float> _prune_elapsed;            // per-spore deadline, -1 = immortal

	// ---- Depth noise (frontier waviness) ----
	// Per-cell positional noise offsets the effective depth used for
	// sweep sorting and activation, breaking up the straight-line
	// frontier.  Amplitude is in depth-units (default 2.0 = ±2 cells).
	// Frequency scales the input coordinates to the noise hash
	// (default 1.0; higher = more wiggles per unit distance).
	// Set amplitude to 0 to disable; takes effect on next _build_sweep_list().
	float _depth_noise_amplitude = 2.0f;
	float _depth_noise_frequency = 1.0f;

	// ---- Spore handicap (rubber-banding) ----
	// When the player pulls ahead of the spore front, spores temporarily
	// speed up.  The extra depth they cover is stored as handicap.
	// Later, when the player is close, spores slow down to "pay back"
	// the handicap, so total clear time is unaffected.
	float _spore_handicap = 0.0f;
	int _player_chamber = -1;                // set by GDScript each frame (min across players in MP)
	int _catch_up_threshold = 1;             // chambers ahead to trigger catch-up
	float _catch_up_speed_multiplier = 5.0f; // speed multiplier during catch-up
	float _payback_speed_multiplier = 0.5f;  // speed multiplier during payback

	// ---- Chamber 0 speed boost ----
	// Chamber 0 is the starting chamber; its progress sets the pace for
	// the early game.  This multiplier is applied on top of the per-chamber
	// speed set via set_chamber_speed().  Set to 1.0 for no change.
	float _chamber_zero_speed_multiplier = 4.0f;

	// Per-spore shrink state for visual removal.
	// _shrink_times[id] = total_time when shrinking began; -1 if not shrinking.
	// _shrink_start_radii[id] = radius at the moment shrinking started.
	Vector<float> _shrink_times;
	Vector<float> _shrink_start_radii;

	// ---- Client mode (multiplayer) ----
	// When true, the SporeManager runs on a non-authority peer.  It
	// computes radii and applies ward force-limits.  Spore despawns
	// (shrink-to-free, lifetime death, overlap pruning) are
	// deterministic — both host and client reach the same conclusions
	// because spawn positions, spawn times, and sweep depth are synced.
	bool _client_mode = false;

	// ---- Cell graph (replaces GDScript spore_loc Dictionary) ----
	// All cells live in one combined HashMap.  Chambers share the same
	// depth map so ward-blocking affects all chambers uniformly.
	// Entry / exit cells are per-chamber for sweep speed lookup.
	struct Cell {
		Vector3i grid_key;
		Vector3 world_pos;               // precomputed from grid_key / spore_res
		int32_t depth = -1;              // -1 = unvisited, INF-like marker = unreachable
		int32_t chamber_id = -1;
		bool spawned = false;            // has _activate_spore_cell been called?
		bool blocked_by_ward = false;    // inside an active ward → depth treated as INF
		bool dead = false;               // zone 1: behind sweep, all neighbors spawned/blocked, immutable
		float depth_noise = 0.0f;        // normalized [-1,1] positional noise for frontier waviness
	};

	HashMap<Vector3i, Cell> _cells;

	// BFS seed points per chamber.
	// Entry cells (chamber entrance) seed the BFS at depth 0.
	// Exit cells are used to compute the initial chamber speed.
	HashMap<int32_t, Vector<Vector3i>> _entry_cells;
	HashMap<int32_t, Vector<Vector3i>> _exit_cells;

	// Per-chamber sweep speed (depth-units per second).
	// Computed once at init from consume_time and entry→exit depth.
	// During sweep, speed changes as the cursor crosses chamber boundaries.
	HashMap<int32_t, float> _chamber_speeds;

	// Combined depth-sorted sweep list (all chambers merged).
	// Sorted by depth ascending.  Sweep cursor advances through this list,
	// switching speed when cells change chamber.
	Vector<Vector3i> _sorted_cells;
	float _sweep = 1.0f;               // current sweep cursor (depth-units)
	int32_t _sweep_idx = 0;            // index into _sorted_cells
	bool _sweep_dirty = true;          // force rebuild after init / ward change

	// Grid resolution (mirrors GDScript spore_res constant, default 1.0).
	float _spore_res = 1.0f;

	// Manual start cell for BFS seeding (set via GDScript when the
	// first chamber has no level_in_pos, e.g. pre-placed chamber_0).
	// When set, this cell seeds the BFS at depth 0 regardless of
	// entry cell configuration.
	Vector3i _start_cell;
	bool _has_start_cell = false;

	// BFS neighbours: 124 Chebyshev-distance-2 offsets so the flood-fill
	// bridges 1-cell gaps in the collision geometry.
	Vector<Vector3i> _bfs_neighbors;
	void _init_bfs_neighbors();

	// Incremental BFS state (lazy depth computation).
	// When the sweep cursor approaches computed_max_depth, we run
	// another BFS wave to look ahead LOOKAHEAD depth-units.
	float _bfs_computed_depth = 0.0f;
	// Maximum depth ever reached by any BFS run.  Monotonically
	// increases; never reset by ward operations.  Used as the
	// so we always know the true extent of the graph.
	float _bfs_global_max_depth = 0.0f;
	bool _cells_added = false;
	// Cells known to have at least one unvisited (depth=-1) neighbour.
	// Updated incrementally by add_cell(), _run_bfs_incremental(),
	// and on_wards_changed().  Eliminates the O(cells × 124) frontier
	// scan that used to run on every BFS extension.
	HashSet<Vector3i> _frontier_set;

	// Spore frontier: cells that have spawned AND have at least one
	// unspawned, unblocked neighbor.  These are the permanent BFS
	// anchor points for any re-flood after ward changes.  Dead cells
	// (all neighbors spawned/blocked) are excluded and never touched again.
	HashSet<Vector3i> _spore_frontier;
	static constexpr float BFS_LOOKAHEAD = 20.0f;

	// ---- BFS / sweep helpers ----
	void _run_bfs_incremental(float p_target_depth);
	void _run_clean_bfs_from(); // Reset AHEAD depths and re-flood from spore frontier.
	void _build_sweep_list();

	enum class CellZone {
		DEAD,
		SPORE_FRONTIER,
		AHEAD,
	};
	CellZone _classify_cell(const Vector3i &p_key) const;
	void _update_spore_frontier_for_spawn(const Vector3i &p_key);

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
	Vector<Vector3i> _query_cells_in_ward_sphere(const Vector3 &p_center, float p_radius) const;
	bool _is_position_warded(const Vector3 &p_pos) const;
	void _detect_overlaps(double p_total_time);
	void _assign_prune_deadline(int32_t p_id, const Vector3 &p_pos, float p_spawn_depth);

protected:
	static void _bind_methods();

public:
	SporeManager();
	~SporeManager();

	// ---- Spore lifecycle (called from GDScript) ----
	int32_t add_spore(const Vector3 &p_pos, int p_profile = PROFILE_NORMAL, int p_chamber_id = -1, float p_spawn_depth = -1.0f);
	// Overload that accepts an explicit spawn_time (used by RPC on clients).
	int32_t add_spore_with_time(const Vector3 &p_pos, int p_profile, int p_chamber_id, float p_spawn_depth, double p_spawn_time);
	void remove_spore(int32_t p_id);
	void remove_spores_in_chamber(int p_chamber_id, bool p_shrink_first = false);
	void shrink_spore(int32_t p_id);
	void shrink_spores_in_chamber(int p_chamber_id);
	void set_spore_state(int32_t p_id, int p_state);
	int get_spore_state(int32_t p_id) const;
	void set_spore_profile(int32_t p_id, int p_profile);
	int get_spore_profile(int32_t p_id) const;
	void set_start_delay(float p_delay);
	float get_start_delay() const;

	// ---- Growth phase timing & radius caps ----
	void set_max_radius_normal(float p_radius);
	float get_max_radius_normal() const;
	void set_max_radius_strain(float p_radius);
	float get_max_radius_strain() const;
	void set_phase1_duration(float p_duration);
	float get_phase1_duration() const;
	void set_phase2_duration(float p_duration);
	float get_phase2_duration() const;
	void set_phase3_duration(float p_duration);
	float get_phase3_duration() const;

	// ---- Client mode (multiplayer) ----
	void set_client_mode(bool p_enabled);
	bool is_client_mode() const;

	// ---- Per-frame update ----
	void update(double p_delta, double p_total_time);

	// ---- Read current state (GDScript queries) ----
	float get_spore_radius(int32_t p_id) const;
	Vector3 get_spore_position(int32_t p_id) const;
	bool is_spore_alive(int32_t p_id) const;
	int get_spore_count() const;
	int get_active_spore_count() const;

	// ---- Diagnostic queries ----
	PackedVector3Array get_all_spore_positions() const;

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

	// ---- Depth-based lifecycle acceleration config ----
	void set_depth_lifecycle_enabled(bool p_enabled);
	bool is_depth_lifecycle_enabled() const;
	void set_depth_lifecycle_mid_threshold(float p_threshold);
	float get_depth_lifecycle_mid_threshold() const;
	void set_depth_lifecycle_full_threshold(float p_threshold);
	float get_depth_lifecycle_full_threshold() const;
	void set_depth_lifecycle_mid_multiplier(float p_multiplier);
	float get_depth_lifecycle_mid_multiplier() const;

	// ---- Mature phase & overlap cleanup config ----
	void set_mature_phase_enabled(bool p_enabled);
	bool is_mature_phase_enabled() const;
	void set_overlap_cleanup_enabled(bool p_enabled);
	bool is_overlap_cleanup_enabled() const;
	void set_overlap_shrink_fraction(float p_fraction);
	float get_overlap_shrink_fraction() const;
	void set_overlap_shrink_duration(float p_duration);
	float get_overlap_shrink_duration() const;
	void set_overlap_radius(float p_radius);
	float get_overlap_radius() const;
	void set_overlap_interval(float p_interval);
	float get_overlap_interval() const;
	void set_overlap_min_count(int p_count);
	int get_overlap_min_count() const;

	// ---- Prune-based lifecycle config ----
	void set_prune_enabled(bool p_enabled);
	bool is_prune_enabled() const;
	void set_prune_fraction_immortal(float p_fraction);
	float get_prune_fraction_immortal() const;
	void set_prune_mean_elapsed(float p_mean);
	float get_prune_mean_elapsed() const;
	void set_prune_min_elapsed(float p_min);
	float get_prune_min_elapsed() const;

	// ---- Depth noise (frontier waviness) ----
	void set_depth_noise_amplitude(float p_amplitude);
	float get_depth_noise_amplitude() const;
	void set_depth_noise_frequency(float p_frequency);
	float get_depth_noise_frequency() const;

	// ---- Spore handicap (rubber-banding) ----
	void set_player_chamber(int p_chamber);
	float get_spore_handicap() const;
	void set_catch_up_threshold(int p_threshold);
	int get_catch_up_threshold() const;
	void set_catch_up_speed_multiplier(float p_multiplier);
	float get_catch_up_speed_multiplier() const;
	void set_payback_speed_multiplier(float p_multiplier);
	float get_payback_speed_multiplier() const;

	// ---- Chamber 0 speed boost ----
	void set_chamber_zero_speed_multiplier(float p_multiplier);
	float get_chamber_zero_speed_multiplier() const;

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
	// Returns alternating (age, radius) for each spore in the chamber.
	// GDScript uses this for INSTANCE_CUSTOM on the MultiMesh to drive the
	// spore fragment shader.
	PackedFloat32Array get_spore_ages_for_chamber(int p_chamber_id) const;
	// Returns interleaved transform + color: 16 floats per spore.
	// [0..11] = 3x4 transform (scale=radius), [12..15] = (age, radius, 0, 1).
	// GDScript feeds this directly to MultiMesh.set_buffer() with use_colors=true.
	PackedFloat32Array get_spore_buffer_for_chamber(int p_chamber_id) const;
	int get_spore_chamber(int32_t p_id) const;

	// ---- Cell graph (replaces GDScript spore_loc / depth / sweep logic) ----

	// Called during shape rendering: add a cell to the grid.
	// If a cell already exists at grid_key, chamber_id is updated.
	void add_cell(const Vector3i &p_grid_key, const Vector3 &p_world_pos, int p_chamber_id);
	void remove_cell(const Vector3i &p_grid_key);
	bool has_cell(const Vector3i &p_grid_key) const;

	// Set entry/exit cells for a chamber (called after shape rendering).
	void set_chamber_entry_cells(int p_chamber_id, const TypedArray<Vector3i> &p_keys);
	void set_chamber_exit_cells(int p_chamber_id, const TypedArray<Vector3i> &p_keys);

	// Set per-chamber sweep speed (computed from consume_time at init).
	void set_chamber_speed(int p_chamber_id, float p_speed);
	float get_chamber_speed(int p_chamber_id) const;

	// Run the full BFS flood-fill from all entry cells.
	// Assigns depth to every reachable cell, respects ward-blocked cells.
	// Called once after all shapes are rendered, and again after ward changes.
	void propagate_depths();

	// Advance the sweep cursor by delta seconds.  Returns a Dictionary
	// mapping chamber_id → Array[Vector3i] of newly activated grid keys.
	// GDScript loops over the result and calls _activate_spore_cell for each.
	Dictionary advance_sweeps(float p_delta);

	// Mark a cell as spawned (called by GDScript after _activate_spore_cell).
	void mark_cell_spawned(const Vector3i &p_grid_key);

	// Called after ward positions/radii change.  Re-runs the BFS so that
	// blocked cells are pushed to unreachable depth and unblocked cells
	// recover their natural depth.
	void on_wards_changed();

	// Per-ward activation: called when a single ward is placed or its
	// radius increases.  Only touches cells inside the ward sphere
	// (O(ward_volume) instead of O(all_cells)).
	void on_ward_activated(const Vector3 &p_center, float p_radius);

	// Per-ward deactivation: called when a ward is removed or its
	// radius decreases.  Classifies unblocked cells by neighbor type
	// and runs the minimum necessary BFS.
	void on_ward_deactivated(const Vector3 &p_center, float p_radius);

	// GDScript queries for cell state.
	int get_cell_depth(const Vector3i &p_grid_key) const;
	int get_cell_chamber(const Vector3i &p_grid_key) const;
	bool is_cell_blocked(const Vector3i &p_grid_key) const;

	// Current sweep cursor (depth-units).  Exposed so GDScript can
	// decide when chambers are safely behind the frontier.
	float get_sweep_depth() const;

	// Set the sweep cursor directly (used on client peers to sync
	// lifecycle acceleration from the host).  Only meaningful when
	// depth_lifecycle_enabled is true.
	void set_sweep(float p_sweep);

	// Grid resolution (mirrors GDScript spore_res).
	void set_spore_res(float p_res);
	float get_spore_res() const;

	// Manual BFS start cell.  When set, the BFS seeds from this cell
	// at depth 0 instead of (or in addition to) entry cells.
	// Used when the first chamber has no level_in_pos (e.g. chamber_0).
	void set_start_cell(const Vector3i &p_grid_key);

	// Called by GDScript after syncing new cells (replaces full
	// propagate_depths() for incremental chamber loading).
	void notify_cells_added();

// Signal that depths should be (re)computed on the next
// advance_sweeps() call.  Previously ran a full-graph BFS;
// now just sets _cells_added so the lazy BFS extension handles
// it incrementally — no deep traversal needed.
	void ensure_depths_computed();

	// BFS neighbour count (for debugging / profiling).
	int get_bfs_neighbor_count() const;
};

VARIANT_ENUM_CAST(SporeManager::SporeState);
VARIANT_ENUM_CAST(SporeManager::Profile);
