# Ward → BFS Refactor: Zone-Based Cell Classification

## Goal

Replace the current O(all_cells × 5 + all_cells × 124) `on_wards_changed()` with a
zone-based approach that only touches cells affected by ward changes.  Dead cells
(behind the sweepline) are excluded from all loops permanently.

---

## 1. Three-Zone Model

Every cell falls into exactly one zone relative to the spore sweep cursor `_sweep`:

```
ZONE 1: DEAD              ZONE 2: SPORE FRONTIER        ZONE 3: AHEAD
depth settled,            spawns happening now,          undiscovered or
spores alive & mature     sweep passing through          ahead of BFS frontier
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
spawned=true              spawned=true OR effective      depth=-1, or
AND all neighbors         depth close to _sweep          depth > sweep but
are spawned or blocked    AND has ≥1 unspawned,          not yet spawned
                          unblocked neighbor
```

### Invariants

| Zone | Can depth change? | Can `blocked_by_ward` change? | Present in loops? |
|------|-------------------|-------------------------------|-------------------|
| DEAD | **No** (irreversible) | **No** (only via ward radius change, handled as deactivation+reactivation) | **Never** |
| SPORE FRONTIER | **No** (anchored) | Yes, if ward overlaps | Yes, only when ward overlaps |
| AHEAD | Yes (BFS recomputes) | Yes, if ward overlaps | Yes |

### Spore Frontier Definition (precise)

A cell is in the **spore frontier** iff:

```
spawned == true
AND exists at least one neighbor n such that:
    n exists in _cells
    AND n.blocked_by_ward == false
    AND n.spawned == false
```

This matches the definition we discussed: cells that have already birthed spores
and border cells that haven't.  These are the BFS anchor points for any re-flood.

### Dead Cell Definition

```
spawned == true
AND for all 124 neighbors n:
    n does NOT exist in _cells  OR  n.blocked_by_ward == true  OR  n.spawned == true
```

Once a cell and all its reachable neighbors have spawned, no computation will
ever change anything about it.

---

## 2. Data Structure Additions

### 2.1 New Cell field

```cpp
// In struct Cell (spore_manager.h):
bool dead = false;  // zone 1 — excluded from all loops, never revisited
```

Set `dead = true` when a cell transitions out of the spore frontier (i.e., all
its neighbors have spawned or become blocked).  This is a one-way transition.

### 2.2 Spore Frontier Set

```cpp
HashSet<Vector3i> _spore_frontier;  // zone 2 cells — BFS anchor points
```

Maintained incrementally:
- **Add** when a cell spawns and has ≥1 unspawned/unblocked neighbor.
- **Remove** when all its neighbors have spawned or become blocked → promote to dead.
- **Remove** when a ward blocks it → handled by ward activation logic.

### 2.3 Dead-Count Tracking (optional, for debugging)

```cpp
int _dead_cell_count = 0;
int _frontier_cell_count = 0;
int _ahead_cell_count = 0;  // live cells - dead - frontier
```

---

## 3. Zone Classification Function

```cpp
enum class CellZone {
    DEAD,
    SPORE_FRONTIER,
    AHEAD
};

CellZone SporeManager::_classify_cell(const Cell &c) const {
    if (c.dead) return CellZone::DEAD;
    if (!c.spawned) return CellZone::AHEAD;

    // spawned && !dead → check if it's still on the frontier
    for (const Vector3i &n : _bfs_neighbors) {
        Vector3i nk = c.grid_key + n;
        const Cell *nc = _cells.getptr(nk);
        if (nc && !nc->blocked_by_ward && !nc->spawned) {
            return CellZone::SPORE_FRONTIER;
        }
    }
    // All reachable neighbors are spawned or blocked → dead
    return CellZone::DEAD;
}
```

**Note**: This function does 124 hash lookups per call.  We do NOT call it in
hot loops.  It is called only:
- When a cell spawns (to decide spore_frontier vs dead).
- When a ward changes status of cells it overlaps (bounded by ward volume).

---

## 4. Spore Frontier Maintenance

### 4.1 On Cell Spawn (`advance_sweeps` or `mark_cell_spawned`)

When a cell transitions from `spawned=false` to `spawned=true`:

```
1. Classify the cell using _classify_cell().
2. If SPORE_FRONTIER → insert into _spore_frontier.
3. If DEAD → set c.dead = true.
4. For each of the cell's 124 neighbors that are in _spore_frontier:
     Re-classify that neighbor.  If it's now DEAD:
       Remove from _spore_frontier, set dead = true.
```

This ensures `_spore_frontier` is always exactly the set of anchor cells.

### 4.2 Initial State (first frame)

Before any cells have spawned:
- `_spore_frontier` is empty.
- `_frontier_set` (BFS frontier) is built as before from entry cells / start cell.
- The sweep advances; as cells spawn, `_spore_frontier` fills in.

If a ward change happens and `_spore_frontier` is empty (very early game),
fall back to the current behavior: re-BFS from entry cells / start cell.

---

## 5. Ward Activation Flow (New)

When a ward is placed or its radius increases:

```
on_ward_activated(ward_pos, ward_radius):
    affected_cells = query_cells_in_sphere(ward_pos, ward_radius)
    // ^^^ Uses _ward_grid for O(ward_volume) lookup, NOT O(all_cells).

    any_frontier_affected = false
    cells_to_block = []

    for cell in affected_cells:
        zone = _classify_cell(cell)
        switch zone:
            case DEAD:
                // Ignore.  Dead cells are immutable.
                continue

            case SPORE_FRONTIER:
                any_frontier_affected = true
                cells_to_block.push_back(cell)
                // Will need to re-BFS because the sweep anchor is blocked.

            case AHEAD:
                cells_to_block.push_back(cell)
                // Mark blocked; depth will be reset.  If the cell had
                // depth >= 0 and is ahead of spore frontier, the BFS
                // frontier may need adjustment but no full re-BFS needed
                // unless spore frontier is also affected.

    if cells_to_block is empty:
        return  // Ward fully inside dead zone — nothing to do.

    // Apply blocking.
    for cell in cells_to_block:
        cell.blocked_by_ward = true
        if cell in _spore_frontier:
            _spore_frontier.erase(cell.grid_key)
        if cell in _frontier_set:
            _frontier_set.erase(cell.grid_key)
        // Reset depth so BFS doesn't use stale values.
        cell.depth = -1

    // ---- "Behind reset" (stale depth sweep) ----
    // Only needed if any_frontier_affected is true.
    // Find the minimum depth among newly-blocked cells that had depth >= 0.
    // Reset all AHEAD cells with depth >= min_blocked_depth.
    // (This prevents spores walking through wards via stale paths.)

    if any_frontier_affected:
        min_blocked_depth = min depth among cells_to_block where old_depth >= 0
        if min_blocked_depth < INT_MAX:
            for each cell in _cells where !dead && !blocked:
                if cell.depth >= min_blocked_depth:
                    cell.depth = -1
            // Rebuild _frontier_set from _spore_frontier + entry cells.
            _run_clean_bfs_from(_spore_frontier)

    else:
        // Only AHEAD cells affected.  No behind-reset needed.
        // Just remove affected cells from _frontier_set.
        // BFS will naturally route around them on next lazy extension.
        // No BFS run needed now.
        pass

    _sweep_dirty = true
```

### Key difference from current code

| Aspect | Current | New |
|--------|---------|-----|
| Cells examined | ALL (HashMap iteration) | Only ward_volume (grid query) |
| Dead cells touched | Yes, every time | Never |
| BFS triggered | Always | Only if spore_frontier affected |
| Frontier rebuild | Full O(cells × 124) scan | Seeded from _spore_frontier |

---

## 6. Ward Deactivation Flow (New)

When a ward is removed or its radius decreases:

```
on_ward_deactivated(ward_pos, ward_radius):
    affected_cells = query_cells_in_sphere(ward_pos, ward_radius)

    cells_to_unblock = []
    has_dead_neighbor_cells = []   // need depth = sweep, seed BFS
    has_visited_neighbor_cells = [] // need BFS frontier, lazy BFS
    isolated_cells = []            // no neighbors with depth, just unblock

    for cell in affected_cells:
        if cell is DEAD:
            continue  // Dead cells stay dead; their blocked status is irrelevant.

        if not cell.blocked_by_ward:
            continue  // Not actually blocked (possibly already handled).

        cells_to_unblock.push_back(cell)

        // Classify by neighbor types.
        has_dead_neighbor = false
        has_visited_neighbor = false

        for each of 124 neighbors n:
            if n exists and n is DEAD:
                has_dead_neighbor = true
                break  // Dead neighbor takes priority.
            if n exists and !n.blocked_by_ward and n.depth >= 0:
                has_visited_neighbor = true
                // Don't break; dead neighbor check takes priority.

        if has_dead_neighbor:
            has_dead_neighbor_cells.push_back(cell)
        elif has_visited_neighbor:
            has_visited_neighbor_cells.push_back(cell)
        else:
            isolated_cells.push_back(cell)

    if cells_to_unblock is empty:
        return

    // Unblock all.
    for cell in cells_to_unblock:
        cell.blocked_by_ward = false
        cell.depth = -1  // Will be recomputed.

    // ---- Case 1: Dead-neighbor cells → full BFS from spore frontier ----
    for cell in has_dead_neighbor_cells:
        cell.depth = (int)_sweep  // Anchor at current sweep depth.
        _spore_frontier.insert(cell.grid_key)

    if has_dead_neighbor_cells is not empty:
        _run_clean_bfs_from(_spore_frontier)
        _sweep_dirty = true
        return

    // ---- Case 2: Visited-neighbor cells → lazy BFS extension ----
    for cell in has_visited_neighbor_cells:
        _frontier_set.insert(cell.grid_key)

    if has_visited_neighbor_cells is not empty:
        // Run one BFS wave from the expanded frontier.
        _run_bfs_incremental(_bfs_computed_depth + BFS_LOOKAHEAD)
        _sweep_dirty = true
        return

    // ---- Case 3: Isolated cells → no BFS needed ----
    // Cells unblocked but have no neighbors with known depth.
    // They'll be discovered naturally by future lazy BFS extensions
    // when the spore frontier reaches their vicinity.
    // Nothing to do.
```

### `_run_clean_bfs_from(spore_frontier)` (new helper)

```cpp
void SporeManager::_run_clean_bfs_from(const HashSet<Vector3i> &p_seeds) {
    // Clear BFS state.
    _frontier_set.clear();

    // Reset all AHEAD cells' depth to -1.
    for (auto &E : _cells) {
        if (!E.value.dead && !E.value.blocked_by_ward) {
            E.value.depth = -1;
        }
    }

    // Re-seed BFS from spore frontier cells + entry cells + start cell.
    // Spore frontier cells keep their current depth (anchor).
    // Entry cells get depth 0.
    // Then run BFS to target depth.

    // ... (similar to current _run_bfs_incremental seeding, but
    //      spore_frontier cells are the primary seeds)

    _run_bfs_incremental(MAX(_sweep + BFS_LOOKAHEAD * 2, _bfs_computed_depth + BFS_LOOKAHEAD));
}
```

---

## 7. Moving Wards

For now, moving wards are processed as deactivation + reactivation on a
cooldown (e.g., every 2 seconds).  This avoids per-frame O(ward_volume)
processing when a ward is on a moving platform.

```gdscript
# In main.gd or ward entity:
var _last_ward_move_process_time = 0.0
const WARD_MOVE_PROCESS_INTERVAL = 2.0

func _process_ward_movement():
    if total_time - _last_ward_move_process_time < WARD_MOVE_PROCESS_INTERVAL:
        return
    _last_ward_move_process_time = total_time
    # Remove old position, add new position.
    spore_manager.on_ward_deactivated(old_pos, radius)
    spore_manager.on_ward_activated(new_pos, radius)
```

Alternatively, the C++ side can detect position changes and batch them
internally using the same cooldown.

---

## 8. Dead Cell Exclusion (Loop Guards)

Every existing loop that iterates `_cells` must gain a `if (cell.dead) continue;`
guard.  The affected functions:

| Function | Change |
|----------|--------|
| `_build_sweep_list()` | Skip dead cells when collecting entries. |
| `_run_bfs_incremental()` | Skip dead cells in frontier seeding and visited rebuild. |
| `on_wards_changed()` → `on_ward_activated/deactivated` | Replaced entirely; dead cells implicitly skipped via grid query. |
| `propagate_depths()` | Skip dead cells (they keep their depth). |
| `advance_sweeps()` | Already skips spawned cells; dead cells are a subset. Add guard for clarity. |
| `ensure_depths_computed()` | Skip dead cells. |
| Any debug/query iteration | Guard for safety. |

**Important**: The `_cells` HashMap is NOT modified (no removals).  Dead cells
stay in the map with `dead = true`.  This avoids iterator invalidation and
thread-safety issues.

---

## 9. `_build_sweep_list` Optimization

Currently sorts ALL cells with `depth >= 0 && !blocked`.  With dead cells skipped:

```cpp
for (const auto &E : _cells) {
    if (E.value.dead) continue;  // NEW
    if (E.value.depth >= 0 && !E.value.blocked_by_ward) {
        entries.emplace_back(effective, E.key);
    }
}
```

The sweep_idx skip logic (skip past spawned cells) still works and is even
faster because dead cells are excluded from the sorted list entirely.

---

## 10. `propagate_depths()` Change

Currently resets ALL depths to -1 and does a full BFS to 10000.  With zones:

```cpp
void SporeManager::propagate_depths() {
    // Only reset AHEAD and frontier cells.  Dead cells keep their depth.
    for (auto &E : _cells) {
        if (!E.value.dead) {
            E.value.depth = -1;
        }
    }
    _frontier_set.clear();
    _spore_frontier.clear();  // Will be rebuilt as cells re-spawn (or keep it if cells are still spawned).
    _run_bfs_incremental(10000.0f);
    _build_sweep_list();
}
```

---

## 11. Ward Spatial Query: `query_cells_in_sphere()`

New helper to find all cells inside a ward's radius using the ward grid:

```cpp
Vector<Vector3i> SporeManager::_query_cells_in_ward_sphere(
    const Vector3 &p_center, float p_radius
) const {
    Vector<Vector3i> result;
    // Use _ward_grid to find candidate cells.
    // For each grid cell in [center - radius, center + radius] / WARD_CELL_SIZE:
    //   Check each cell in that bucket for distance < radius.
    // This is O(ward_volume / WARD_CELL_SIZE³) ≈ O(affected cells), not O(all cells).
    return result;
}
```

Alternatively, since wards and cells share the same spatial domain, we can
iterate the ward grid buckets that overlap the ward's sphere and check the
cells registered in those buckets.  If cells aren't currently registered in
`_ward_grid`, we need a separate cell spatial index (see Section 12).

---

## 12. Cell Spatial Index (New)

The current `_ward_grid` maps `grid_key → ward_ids`.  We need the reverse:
a grid that maps `grid_key → cell_keys` for fast ward overlap queries.

```cpp
// In spore_manager.h:
HashMap<Vector3i, Vector<Vector3i>> _cell_grid;
static constexpr float CELL_GRID_SIZE = 4.0f;  // Same as WARD_CELL_SIZE

void _rebuild_cell_grid();  // Called after batch cell additions.
```

When `add_cell()` is called, also insert into `_cell_grid`.  When a cell becomes
dead, it can optionally be removed from the grid (or left in — the dead check
in the query will filter it out).

Actually, a simpler approach: just use the `_ward_grid` grid key size and
iterate the 3×3×3 neighborhood, but query `_cells` instead of `_wards`.  The
ward sphere query becomes:

```cpp
Vector<Vector3i> _query_cells_in_ward_sphere(Vector3 center, float radius) {
    Vector<Vector3i> result;
    float r2 = radius * radius;
    Vector3i min_k = world_to_grid(center - Vector3(radius, radius, radius));
    Vector3i max_k = world_to_grid(center + Vector3(radius, radius, radius));
    for (int x = min_k.x; x <= max_k.x; x++) {
        for (int y = min_k.y; y <= max_k.y; y++) {
            for (int z = min_k.z; z <= max_k.z; z++) {
                Vector3i gk(x, y, z);
                const Vector<int32_t> *cell_ids = _cell_grid.getptr(gk);
                if (!cell_ids) continue;
                for (int32_t cell_idx : *cell_ids) {
                    // cell_positions[cell_idx] is the world pos
                    // But we store cells in a HashMap, not a vector...
                }
            }
        }
    }
}
```

**Simpler approach (no new grid)**: Since `_cells` is a `HashMap<Vector3i, Cell>`,
and cells are on a regular grid with spacing `_spore_res` (1.0), we can directly
enumerate candidate grid keys inside the ward sphere:

```cpp
Vector<Vector3i> _query_cells_in_ward_sphere(Vector3 center, float radius) {
    Vector<Vector3i> result;
    float r2 = radius * radius;
    // Cells are on a grid with spacing _spore_res.
    // Enumerate all integer grid positions within the sphere.
    int r_cells = (int)ceil(radius / _spore_res);
    Vector3i center_cell(
        (int)round(center.x / _spore_res),
        (int)round(center.y / _spore_res),
        (int)round(center.z / _spore_res)
    );
    for (int dx = -r_cells; dx <= r_cells; dx++) {
        for (int dy = -r_cells; dy <= r_cells; dy++) {
            for (int dz = -r_cells; dz <= r_cells; dz++) {
                Vector3i key = center_cell + Vector3i(dx, dy, dz);
                const Cell *c = _cells.getptr(key);
                if (!c) continue;
                // The cell's world_pos should be within the sphere.
                if (c->world_pos.distance_squared_to(center) <= r2) {
                    result.push_back(key);
                }
            }
        }
    }
    return result;
}
```

This is O(ward_volume / spore_res³) hash lookups — for a `radius = 10` ward,
that's ~4,200 lookups (worst-case cube), but `_cells.getptr` only succeeds for
cells that actually exist.  Most grid positions are empty.  This is still
orders of magnitude cheaper than O(all_cells × 124).

---

## 13. Migration Steps (Implementation Order)

### Step 1: Add `dead` field and `_spore_frontier` set
- Add `bool dead = false` to `Cell` struct.
- Add `HashSet<Vector3i> _spore_frontier` to SporeManager.
- No behavior changes yet.  `dead` defaults to false, everything works as before.

### Step 2: Implement spore frontier maintenance
- In `advance_sweeps()` / `mark_cell_spawned()`, after setting `spawned = true`:
  - Call `_classify_cell()`.  If SPORE_FRONTIER, add to `_spore_frontier`.
  - Re-classify neighbors that were in `_spore_frontier`.
- Verify `_spore_frontier` size matches expectations.

### Step 3: Add dead cell guards to all loops
- `_build_sweep_list()`: `if (cell.dead) continue;`
- `_run_bfs_incremental()`: skip dead in frontier seed + visited rebuild.
- `advance_sweeps()`: `if (cell.dead) continue;`
- `propagate_depths()`: don't reset dead cells.
- Verify no regressions (dead cells should be 0 until step 4).

### Step 4: Implement `_query_cells_in_ward_sphere()`
- Add the grid enumeration helper.
- Test independently.

### Step 5: Implement `on_ward_activated()` (replaces blocking half of `on_wards_changed`)
- Use `_query_cells_in_ward_sphere()` instead of iterating all cells.
- Apply zone-based logic as specified in Section 5.
- Keep old `on_wards_changed()` as fallback behind a compile flag for A/B testing.

### Step 6: Implement `on_ward_deactivated()` (replaces unblocking half)
- Use zone-based logic as specified in Section 6.
- Handle the three sub-cases.

### Step 7: Implement `_run_clean_bfs_from(spore_frontier)`
- Refactor `_run_bfs_incremental` to accept an optional seed set.
- The clean BFS resets all AHEAD depths and re-floods from anchors.

### Step 8: Moving ward cooldown
- Add cooldown logic in GDScript or C++.

### Step 9: Remove old `on_wards_changed()` and cleanup
- Delete the 5-pass code path.
- Remove `old_blocked` HashMap, `behind_reset` logic, etc.

### Step 10: Profile and tune
- Measure ward activation/deactivation latency with large cell graphs.
- Tune `BFS_LOOKAHEAD` if needed.
- Consider adding the dead-cell promition in `_build_sweep_list` (cells behind
  `_sweep_idx` that are fully spawned can be bulk-promoted to dead).

---

## 14. Edge Cases

### 14.1 Ward covers entire spore frontier
If a ward blocks ALL spore frontier cells (e.g., giant ward near the player),
`_spore_frontier` becomes empty and the BFS has nothing to seed from.
**Fallback**: Re-BFS from entry cells / start cell, same as initial state.

### 14.2 Ward placed before any spores spawned
`_spore_frontier` is empty.  Treat as current behavior — BFS from entry cells.
The ward still blocks AHEAD cells; the BFS routes around them.

### 14.3 Ward overlaps dead cells
Dead cells are immutable.  If a ward expands to cover a dead cell, the dead
cell's `blocked_by_ward` becomes stale (still false).  This is acceptable
because dead cells are never queried again.  The ward's effect on the game
(spore suppression) is handled by `query_sphere()` at the GDScript level,
which queries alive spores, not cells.

### 14.4 Multiple wards changing simultaneously
Batch all ward changes into one activation + one deactivation pass per frame.
Process activations first, then deactivations.  If a cell is both activated
and deactivated in the same frame (ward moved slightly), the net effect is
no change for that cell.

### 14.5 Chamber boundaries
Cells from different chambers share the same `_cells` HashMap.  The zone model
is chamber-agnostic — a dead cell in chamber 3 is just as dead as one in
chamber 1.  The sweep cursor handles per-chamber speed naturally as before.

---

## 15. Expected Performance Impact

| Scenario | Before | After |
|----------|--------|-------|
| Ward placed (anywhere), 10k cells | ~5×10k + 10k×124 = ~1.29M ops | ~4k lookups (ward volume) + maybe BFS |
| Ward removed (dead-neighbor case), 10k cells | ~1.29M ops + BFS | ~4k lookups + BFS from ~100 frontier cells |
| Ward removed (isolated, far ahead), 10k cells | ~1.29M ops + BFS | ~4k lookups, **no BFS** |
| `_build_sweep_list`, 10k cells, 8k dead | 10k log₂(10k) sorts | 2k log₂(2k) sorts (~5× fewer comparisons) |
| Every BFS frontier rebuild | O(visited × 124) | O(visited × 124), but visited set is smaller (no dead cells) |
