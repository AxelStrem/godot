/**************************************************************************/
/*  wfc_solver.cpp                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "wfc_solver.h"

#include "core/math/math_funcs.h"
#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"
#include "core/templates/sort_array.h"

namespace {

// Use a macro instead of static const to avoid static init order issues — SNAME
// requires the StringName system to be configured, which isn't guaranteed at DLL load time.
#define WFC_NONE_CONNECTION SNAME("none")
static const StringName WFC_WALL_PANEL_TYPE = SNAME("wall_panel");
static constexpr int WFC_LOOKUP_RANGE = 3;

static double _usec_to_msec(uint64_t p_usec) {
	return double(p_usec) / 1000.0;
}

static int _count_bits_u64(uint64_t p_value) {
	int count = 0;
	while (p_value != 0) {
		p_value &= p_value - 1;
		count++;
	}
	return count;
}

struct BitMask {
	Vector<uint64_t> words;

	void resize(int p_word_count) {
		words.resize(p_word_count);
		for (int i = 0; i < words.size(); i++) {
			words.write[i] = 0;
		}
	}

	void set_bit(int p_bit) {
		words.write[p_bit / 64] |= uint64_t(1) << (p_bit % 64);
	}

	bool has_bit(int p_bit) const {
		return (words[p_bit / 64] & (uint64_t(1) << (p_bit % 64))) != 0;
	}

	bool is_empty() const {
		for (int i = 0; i < words.size(); i++) {
			if (words[i] != 0) {
				return false;
			}
		}
		return true;
	}

	int count_bits() const {
		int count = 0;
		for (int i = 0; i < words.size(); i++) {
			count += _count_bits_u64(words[i]);
		}
		return count;
	}

	void bit_or(const BitMask &p_other) {
		for (int i = 0; i < words.size(); i++) {
			words.write[i] |= p_other.words[i];
		}
	}

	bool bit_and_in_place(const BitMask &p_other) {
		bool changed = false;
		for (int i = 0; i < words.size(); i++) {
			uint64_t before = words[i];
			words.write[i] &= p_other.words[i];
			changed |= before != words[i];
		}
		return changed;
	}

	BitMask intersected(const BitMask &p_other) const {
		BitMask result;
		result.resize(words.size());
		for (int i = 0; i < words.size(); i++) {
			result.words.write[i] = words[i] & p_other.words[i];
		}
		return result;
	}
};

struct AuthoringOption {
	StringName option;
	float probability = 1.0f;
	bool enabled = true;
	int symmetry_fold = 1;
};

struct AuthoringNeighbor {
	StringName name;
	StringName inv_name;
	StringName type;
	Vector3 offset;
	float wobble = 0.001f;
	Vector3 angular_wobble = Vector3(0.5, 0.5, 0.5);
	bool primary = true;
	StringName rotation_lock;
};

struct AuthoringElement {
	ObjectID node_id;
	StringName type;
	Transform3D global_transform;
	int resolve_priority = 0;
	bool defer_collapse = false;
	Vector<AuthoringOption> options;
	Vector<AuthoringNeighbor> neighbors;
};

struct AuthoringSnapshot {
	Dictionary merged_rules;
	Vector<AuthoringElement> elements;
	float cell_size = 4.0f;
	uint64_t seed = 0;
};

struct ConnectionBuild {
	int from_index = -1;
	int to_index = -1;
	StringName from_side;
	StringName to_side;
};

struct CompiledCatalog {
	struct SideData {
		StringName name;
		BitMask none_options;
		Vector<BitMask> connection_to_options;
		Vector<BitMask> option_to_connections;
	};

	struct TypeData {
		StringName name;
		HashMap<StringName, int> option_ids;
		Vector<StringName> option_names;
		HashMap<StringName, int> side_ids;
		Vector<SideData> sides;
		int option_word_count = 0;
		Vector<int> variant_base_option;
		Vector<int> variant_rotation;
		Vector<StringName> rotation_sides;
	};

	HashMap<StringName, int> type_ids;
	HashMap<StringName, int> connection_ids;
	Vector<StringName> connection_names;
	Vector<TypeData> types;
	int connection_word_count = 0;
	int none_connection_id = -1;
	Vector<BitMask> connection_pair_masks;
};

struct SolvedSide {
	int side_id = -1;
	int other_index = -1;
	int other_side_id = -1;
};

struct SolvedElement {
	ObjectID node_id;
	int type_id = -1;
	int resolve_priority = 0;
	bool defer_collapse = false;
	BitMask options;
	Vector<float> weights;
	int remaining = 0;
	Vector<SolvedSide> sides;
	Vector<int> connected_elements;
	Vector3 position; // debug: grid position
};

struct SolveContext {
	CompiledCatalog catalog;
	Vector<SolvedElement> elements;
	int connection_count = 0;
};

struct SolveResult {
	bool success = false;
	String error;
	Vector<ObjectID> node_ids;
	Vector<StringName> selected_options;
	Vector<int> selected_rotation_steps;
	Vector<PackedStringArray> remaining_options;
	Vector<bool> persist_remaining_options;
	Vector<Dictionary> resolved_data;
	int connection_count = 0;
};

struct SpatialKey {
	int x = 0;
	int y = 0;
	int z = 0;

	static uint32_t hash(const SpatialKey &p_key) {
		return hash_murmur3_one_32(uint32_t(p_key.x)) ^ hash_murmur3_one_32(uint32_t(p_key.y) * 1315423911u) ^ hash_murmur3_one_32(uint32_t(p_key.z) * 2654435761u);
	}

	bool operator==(const SpatialKey &p_other) const {
		return x == p_other.x && y == p_other.y && z == p_other.z;
	}
};

static bool _variant_to_string_name(const Variant &p_value, StringName &r_name) {
	if (p_value.get_type() == Variant::STRING_NAME || p_value.get_type() == Variant::STRING) {
		r_name = StringName(String(p_value));
		return true;
	}
	return false;
}

static bool _variant_is_none_connection(const Variant &p_value) {
	String text = String(p_value).strip_edges();
	return text == "none" || text == "&\"none\"";
}

static Vector3 _object_angle(const Transform3D &p_a, const Transform3D &p_b) {
	Quaternion quat_a = p_a.basis.get_rotation_quaternion();
	Quaternion quat_b = p_b.basis.get_rotation_quaternion();
	Quaternion diff = quat_a.inverse() * quat_b;
	return diff.get_euler();
}

static SpatialKey _make_spatial_key(const Vector3 &p_position, float p_cell_size) {
	SpatialKey key;
	key.x = Math::round(p_position.x / p_cell_size);
	key.y = Math::round(p_position.y / p_cell_size);
	key.z = Math::round(p_position.z / p_cell_size);
	return key;
}

static int _build_connections(const AuthoringSnapshot &p_snapshot, Vector<ConnectionBuild> &r_connections) {
	HashMap<SpatialKey, Vector<int>, SpatialKey> lookup;
	for (int i = 0; i < p_snapshot.elements.size(); i++) {
		const AuthoringElement &element = p_snapshot.elements[i];
		SpatialKey key = _make_spatial_key(element.global_transform.origin, p_snapshot.cell_size);
		lookup[key].push_back(i);
	}

	for (int element_index = 0; element_index < p_snapshot.elements.size(); element_index++) {
		const AuthoringElement &element = p_snapshot.elements[element_index];
		Transform3D inverse = element.global_transform.affine_inverse();
		SpatialKey element_key = _make_spatial_key(element.global_transform.origin, p_snapshot.cell_size);
		for (int neighbor_index = 0; neighbor_index < element.neighbors.size(); neighbor_index++) {
			const AuthoringNeighbor &neighbor = element.neighbors[neighbor_index];
			if (!neighbor.primary) {
				continue;
			}

			int nearest_match = -1;
			float best_distance = FLT_MAX;

			for (int x = element_key.x - WFC_LOOKUP_RANGE; x <= element_key.x + WFC_LOOKUP_RANGE; x++) {
				for (int y = element_key.y - WFC_LOOKUP_RANGE; y <= element_key.y + WFC_LOOKUP_RANGE; y++) {
					for (int z = element_key.z - WFC_LOOKUP_RANGE; z <= element_key.z + WFC_LOOKUP_RANGE; z++) {
						SpatialKey candidate_key;
						candidate_key.x = x;
						candidate_key.y = y;
						candidate_key.z = z;
						if (!lookup.has(candidate_key)) {
							continue;
						}
						const Vector<int> &candidates = lookup[candidate_key];
						for (int candidate_list_index = 0; candidate_list_index < candidates.size(); candidate_list_index++) {
							int candidate_index = candidates[candidate_list_index];
							if (candidate_index == element_index) {
								continue;
							}

							const AuthoringElement &candidate = p_snapshot.elements[candidate_index];
							if (candidate.type != neighbor.type) {
								continue;
							}

							Vector3 local_offset = inverse.xform(candidate.global_transform.origin);
							float distance = local_offset.distance_squared_to(neighbor.offset);
							if (distance > neighbor.wobble) {
								continue;
							}

							Vector3 angle = _object_angle(element.global_transform, candidate.global_transform);
							if (Math::abs(angle.x) > neighbor.angular_wobble.x || Math::abs(angle.y) > neighbor.angular_wobble.y || Math::abs(angle.z) > neighbor.angular_wobble.z) {
								continue;
							}

							if (distance < best_distance) {
								best_distance = distance;
								nearest_match = candidate_index;
							}
						}
					}
				}
			}

			if (nearest_match != -1) {
				ConnectionBuild connection;
				connection.from_index = element_index;
				connection.to_index = nearest_match;
				connection.from_side = neighbor.name;
				connection.to_side = neighbor.inv_name;
				r_connections.push_back(connection);
			}
		}
	}

	return r_connections.size();
}

static bool _compile_catalog(const Dictionary &p_rules, const HashMap<StringName, HashMap<StringName, int>> &p_symmetry_map, CompiledCatalog &r_catalog, String &r_error) {
	r_catalog.none_connection_id = 0;
	r_catalog.connection_ids.insert(WFC_NONE_CONNECTION, 0);
	r_catalog.connection_names.push_back(WFC_NONE_CONNECTION);

	const StringName ROTATION_SIDES_KEY = SNAME("@rotation_sides");
	const StringName CONNECTION_PAIRS_KEY = SNAME("@connection_pairs");

	// Parse @connection_pairs early to register all connection names
	HashMap<StringName, Vector<StringName>> raw_connection_pairs;
	if (p_rules.has(CONNECTION_PAIRS_KEY)) {
		Dictionary pairs_dict = p_rules[CONNECTION_PAIRS_KEY];
		Array pair_keys = pairs_dict.keys();
		for (int pk = 0; pk < pair_keys.size(); pk++) {
			StringName from_conn;
			if (!_variant_to_string_name(pair_keys[pk], from_conn)) {
				continue;
			}
			Vector<StringName> to_list;
			Array to_arr;
			Variant to_val = pairs_dict[pair_keys[pk]];
			if (to_val.get_type() == Variant::ARRAY) {
				to_arr = to_val;
			} else {
				to_arr.append(to_val);
			}
			for (int ti = 0; ti < to_arr.size(); ti++) {
				StringName to_conn;
				if (_variant_to_string_name(to_arr[ti], to_conn)) {
					to_list.push_back(to_conn);
					// Register connection names (don't add "none" — it's already id 0)
					if (!r_catalog.connection_ids.has(to_conn) && to_conn != WFC_NONE_CONNECTION) {
						r_catalog.connection_ids.insert(to_conn, r_catalog.connection_names.size());
						r_catalog.connection_names.push_back(to_conn);
					}
				}
			}
			// Register the from_conn too
			if (!r_catalog.connection_ids.has(from_conn) && from_conn != WFC_NONE_CONNECTION) {
				r_catalog.connection_ids.insert(from_conn, r_catalog.connection_names.size());
				r_catalog.connection_names.push_back(from_conn);
			}
			raw_connection_pairs.insert(from_conn, to_list);
		}
	}

	Array type_keys = p_rules.keys();
	for (int type_index = 0; type_index < type_keys.size(); type_index++) {
		Variant type_variant = type_keys[type_index];
		StringName type_name;
		if (!_variant_to_string_name(type_variant, type_name)) {
			continue;
		}
		// Skip top-level metadata keys
		if (type_name == CONNECTION_PAIRS_KEY) {
			continue;
		}
		CompiledCatalog::TypeData type_data;
		type_data.name = type_name;
		Dictionary type_rules = p_rules[type_name];

		// Read rotation_sides special key
		if (type_rules.has(ROTATION_SIDES_KEY)) {
			Array rotation_sides_arr = type_rules[ROTATION_SIDES_KEY];
			for (int i = 0; i < rotation_sides_arr.size(); i++) {
				StringName side_name;
				if (_variant_to_string_name(rotation_sides_arr[i], side_name)) {
					type_data.rotation_sides.push_back(side_name);
				}
			}
		}

		// First pass: collect option names and side names
		Array side_keys = type_rules.keys();
		for (int side_index = 0; side_index < side_keys.size(); side_index++) {
			Variant side_variant = side_keys[side_index];
			StringName side_name;
			if (!_variant_to_string_name(side_variant, side_name)) {
				continue;
			}
			// Skip special metadata keys
			if (side_name == ROTATION_SIDES_KEY) {
				continue;
			}

			int new_side_index = type_data.sides.size();
			type_data.side_ids.insert(side_name, new_side_index);
			type_data.sides.push_back(CompiledCatalog::SideData());
			type_data.sides.write[new_side_index].name = side_name;

			Array entries = type_rules[side_name];
			for (int entry_index = 0; entry_index < entries.size(); entry_index++) {
				if (entries[entry_index].get_type() != Variant::ARRAY) {
					continue;
				}
				Array entry = entries[entry_index];
				if (entry.size() < 2) {
					continue;
				}
				StringName option_name;
				if (!_variant_to_string_name(entry[0], option_name)) {
					continue;
				}
				if (!type_data.option_ids.has(option_name)) {
					type_data.option_ids.insert(option_name, type_data.option_names.size());
					type_data.option_names.push_back(option_name);
				}

				Array connection_values;
				if (entry[1].get_type() == Variant::ARRAY) {
					connection_values = entry[1];
				} else {
					connection_values.append(entry[1]);
				}

				for (int connection_index = 0; connection_index < connection_values.size(); connection_index++) {
					if (_variant_is_none_connection(connection_values[connection_index])) {
						continue;
					}
					StringName connection_name;
					if (!_variant_to_string_name(connection_values[connection_index], connection_name)) {
						continue;
					}
					if (!r_catalog.connection_ids.has(connection_name)) {
						r_catalog.connection_ids.insert(connection_name, r_catalog.connection_names.size());
						r_catalog.connection_names.push_back(connection_name);
					}
				}

			}
		}

		// Ensure all rotation_sides have side data entries
		for (int i = 0; i < type_data.rotation_sides.size(); i++) {
			const StringName &rs = type_data.rotation_sides[i];
			if (!type_data.side_ids.has(rs)) {
				int new_side_index = type_data.sides.size();
				type_data.side_ids.insert(rs, new_side_index);
				type_data.sides.push_back(CompiledCatalog::SideData());
				type_data.sides.write[new_side_index].name = rs;
			}
		}

		// Expand options with rotation variants
		const HashMap<StringName, int> *symmetry_folds = nullptr;
		if (p_symmetry_map.has(type_name)) {
			symmetry_folds = &p_symmetry_map[type_name];
		}

		for (int base_option = 0; base_option < type_data.option_names.size(); base_option++) {
			const StringName &opt_name = type_data.option_names[base_option];
			int fold = 1;
			if (symmetry_folds != nullptr && symmetry_folds->has(opt_name)) {
				fold = MAX(1, (*symmetry_folds)[opt_name]);
			}
			// Clamp fold: requires rotation_sides to be non-empty
			if (type_data.rotation_sides.is_empty()) {
				fold = 1;
			} else if (fold > 1) {
				// Fold must be a divisor of rotation_sides.size() to tile cleanly
				while (fold > 1 && type_data.rotation_sides.size() % fold != 0) {
					fold--;
				}
			}
			for (int step = 0; step < fold; step++) {
				type_data.variant_base_option.push_back(base_option);
				type_data.variant_rotation.push_back(step);
			}
		}

		int total_variants = type_data.variant_base_option.size();
		type_data.option_word_count = (total_variants + 63) / 64;
		r_catalog.type_ids.insert(type_name, r_catalog.types.size());
		r_catalog.types.push_back(type_data);
	}

	r_catalog.connection_word_count = (r_catalog.connection_names.size() + 63) / 64;

	// Second pass: resize side data arrays for expanded option space
	for (int type_index = 0; type_index < r_catalog.types.size(); type_index++) {
		CompiledCatalog::TypeData &type_data = r_catalog.types.write[type_index];
		int option_count = type_data.variant_base_option.size();
		for (int side_index = 0; side_index < type_data.sides.size(); side_index++) {
			CompiledCatalog::SideData &side_data = type_data.sides.write[side_index];
			side_data.none_options.resize(type_data.option_word_count);
			side_data.connection_to_options.resize(r_catalog.connection_names.size());
			for (int connection_index = 0; connection_index < side_data.connection_to_options.size(); connection_index++) {
				side_data.connection_to_options.write[connection_index].resize(type_data.option_word_count);
			}
			side_data.option_to_connections.resize(option_count);
			for (int variant_index = 0; variant_index < option_count; variant_index++) {
				side_data.option_to_connections.write[variant_index].resize(r_catalog.connection_word_count);
			}
		}
	}

	// Fill side data with rotation-aware remapping
	int rotation_sides_len;
	for (int type_index = 0; type_index < type_keys.size(); type_index++) {
		StringName type_name;
		if (!_variant_to_string_name(type_keys[type_index], type_name)) {
			continue;
		}
		if (!r_catalog.type_ids.has(type_name)) {
			continue;
		}
		CompiledCatalog::TypeData &type_data = r_catalog.types.write[r_catalog.type_ids[type_name]];
		rotation_sides_len = type_data.rotation_sides.size();
		Dictionary type_rules = p_rules[type_name];
		Array side_keys = type_rules.keys();
		for (int side_index = 0; side_index < side_keys.size(); side_index++) {
			StringName side_name;
			if (!_variant_to_string_name(side_keys[side_index], side_name)) {
				continue;
			}
			// Skip special metadata keys
			if (side_name == ROTATION_SIDES_KEY) {
				continue;
			}
			if (!type_data.side_ids.has(side_name)) {
				continue;
			}
			int catalog_side_pos = -1;
			for (int ri = 0; ri < rotation_sides_len; ri++) {
				if (type_data.rotation_sides[ri] == side_name) {
					catalog_side_pos = ri;
					break;
				}
			}

			Array entries = type_rules[side_name];
			for (int entry_index = 0; entry_index < entries.size(); entry_index++) {
				if (entries[entry_index].get_type() != Variant::ARRAY) {
					continue;
				}
				Array entry = entries[entry_index];
				if (entry.size() < 2) {
					continue;
				}
				StringName option_name;
				if (!_variant_to_string_name(entry[0], option_name)) {
					continue;
				}
				if (!type_data.option_ids.has(option_name)) {
					continue;
				}
				int base_option_id = type_data.option_ids[option_name];

				Array connection_values;
				if (entry[1].get_type() == Variant::ARRAY) {
					connection_values = entry[1];
				} else {
					connection_values.append(entry[1]);
				}

				// Apply to all rotation variants of this option
				int variant_count = type_data.variant_base_option.size();
				int fold = 0;
				int first_variant = -1;
				for (int vi = 0; vi < variant_count; vi++) {
					if (type_data.variant_base_option[vi] == base_option_id) {
						if (first_variant == -1) {
							first_variant = vi;
						}
						fold++;
					}
				}

				for (int vi = first_variant; vi < first_variant + fold; vi++) {
					int rotation_step = type_data.variant_rotation[vi];

					// Compute which side this variant presents the connection on
					StringName effective_side = side_name;
					if (catalog_side_pos >= 0 && rotation_step > 0 && rotation_sides_len > 0 && fold > 0) {
						int step_size = rotation_sides_len / fold;
						int remapped_pos = (catalog_side_pos + rotation_step * step_size) % rotation_sides_len;
						effective_side = type_data.rotation_sides[remapped_pos];
					}

					if (!type_data.side_ids.has(effective_side)) {
						continue;
					}
					CompiledCatalog::SideData &effective_side_data = type_data.sides.write[type_data.side_ids[effective_side]];

					for (int connection_index = 0; connection_index < connection_values.size(); connection_index++) {
						if (_variant_is_none_connection(connection_values[connection_index])) {
							effective_side_data.none_options.set_bit(vi);
							// Also populate connection_to_options for "none" so _allowed_options works
							effective_side_data.connection_to_options.write[r_catalog.none_connection_id].set_bit(vi);
							continue;
						}
						StringName connection_name;
						if (!_variant_to_string_name(connection_values[connection_index], connection_name)) {
							continue;
						}
						if (!r_catalog.connection_ids.has(connection_name)) {
							continue;
						}
						int compiled_connection = r_catalog.connection_ids[connection_name];
						effective_side_data.option_to_connections.write[vi].set_bit(compiled_connection);
						effective_side_data.connection_to_options.write[compiled_connection].set_bit(vi);
					}
				}
			}
		}
	}

	// Build connection_pair_masks: default self-pairing, then apply @connection_pairs overrides
	int conn_count = r_catalog.connection_names.size();
	r_catalog.connection_pair_masks.resize(conn_count);
	for (int ci = 0; ci < conn_count; ci++) {
		r_catalog.connection_pair_masks.write[ci].resize(r_catalog.connection_word_count);
		// Default: each connection pairs with itself
		r_catalog.connection_pair_masks.write[ci].set_bit(ci);
	}
	// Apply user overrides
	for (const KeyValue<StringName, Vector<StringName>> &kv : raw_connection_pairs) {
		if (!r_catalog.connection_ids.has(kv.key)) {
			continue;
		}
		int from_id = r_catalog.connection_ids[kv.key];
		r_catalog.connection_pair_masks.write[from_id].words.fill(0); // Clear default self-pairing
		for (const StringName &to_name : kv.value) {
			if (to_name == WFC_NONE_CONNECTION) {
				r_catalog.connection_pair_masks.write[from_id].set_bit(r_catalog.none_connection_id);
			} else if (r_catalog.connection_ids.has(to_name)) {
				r_catalog.connection_pair_masks.write[from_id].set_bit(r_catalog.connection_ids[to_name]);
			}
		}
	}
	// Ensure symmetry: if A pairs with B, B must pair with A
	for (int ci = 0; ci < conn_count; ci++) {
		for (int cj = 0; cj < conn_count; cj++) {
			if (r_catalog.connection_pair_masks[ci].has_bit(cj) && !r_catalog.connection_pair_masks[cj].has_bit(ci)) {
				r_catalog.connection_pair_masks.write[cj].set_bit(ci);
			}
		}
	}

	if (r_catalog.types.is_empty()) {
		r_error = "WFCSolver requires at least one catalog type.";
		return false;
	}

	return true;
}

static BitMask _possible_connections(const CompiledCatalog::TypeData &p_type, int p_side_id, const BitMask &p_options, int p_connection_word_count) {
	BitMask result;
	result.resize(p_connection_word_count);
	const CompiledCatalog::SideData &side_data = p_type.sides[p_side_id];
	int variant_count = p_type.variant_base_option.size();
	for (int vi = 0; vi < variant_count; vi++) {
		if (p_options.has_bit(vi)) {
			result.bit_or(side_data.option_to_connections[vi]);
		}
	}
	return result;
}

static BitMask _allowed_options(const CompiledCatalog::TypeData &p_type, int p_side_id, const BitMask &p_connections) {
	BitMask result;
	result.resize(p_type.option_word_count);
	const CompiledCatalog::SideData &side_data = p_type.sides[p_side_id];
	for (int connection_index = 0; connection_index < side_data.connection_to_options.size(); connection_index++) {
		if (p_connections.has_bit(connection_index)) {
			result.bit_or(side_data.connection_to_options[connection_index]);
		}
	}
	return result;
}

static PackedStringArray _export_allowed_options(const SolvedElement &p_element, const CompiledCatalog::TypeData &p_type_data) {
	PackedStringArray allowed_options;
	HashSet<StringName> seen;
	int variant_count = p_type_data.variant_base_option.size();
	for (int vi = 0; vi < variant_count; vi++) {
		if (p_element.options.has_bit(vi)) {
			int base_id = p_type_data.variant_base_option[vi];
			StringName base_name = p_type_data.option_names[base_id];
			if (!seen.has(base_name)) {
				seen.insert(base_name);
				allowed_options.push_back(String(base_name));
			}
		}
	}
	return allowed_options;
}

static bool _build_graph_snapshot(const AuthoringSnapshot &p_snapshot, const Vector<ConnectionBuild> &p_connections, Ref<WFCGraph> &r_graph, String &r_error) {
	r_graph.instantiate();
	r_graph->set_seed(p_snapshot.seed);

	for (int element_index = 0; element_index < p_snapshot.elements.size(); element_index++) {
		const AuthoringElement &authoring = p_snapshot.elements[element_index];
		Ref<WFCGraphElement> graph_element = memnew(WFCGraphElement);
		graph_element->set_id(int64_t(authoring.node_id));
		graph_element->set_type(authoring.type);
		graph_element->set_global_transform(authoring.global_transform);
		graph_element->set_resolve_priority(authoring.resolve_priority);
		graph_element->set_defer_collapse(authoring.defer_collapse);

		PackedStringArray allowed_options;
		for (int option_index = 0; option_index < authoring.options.size(); option_index++) {
			const AuthoringOption &option = authoring.options[option_index];
			if (option.enabled) {
				allowed_options.push_back(String(option.option));
			}
		}
		graph_element->set_allowed_options(allowed_options);
		r_graph->add_element(graph_element);
	}

	for (int connection_index = 0; connection_index < p_connections.size(); connection_index++) {
		const ConnectionBuild &connection = p_connections[connection_index];
		Ref<WFCGraphElement> from_element = r_graph->get_element(int64_t(p_snapshot.elements[connection.from_index].node_id));
		Ref<WFCGraphElement> to_element = r_graph->get_element(int64_t(p_snapshot.elements[connection.to_index].node_id));
		if (from_element.is_null() || to_element.is_null()) {
			r_error = "WFC graph snapshot lost a connection mapping.";
			return false;
		}
		from_element->set_neighbor_id(connection.from_side, int64_t(p_snapshot.elements[connection.to_index].node_id));
		to_element->set_neighbor_id(connection.to_side, int64_t(p_snapshot.elements[connection.from_index].node_id));
	}

	return true;
}

static bool _apply_graph_to_snapshot(const Ref<WFCGraph> &p_graph, AuthoringSnapshot &r_snapshot, String &r_error) {
	for (int element_index = 0; element_index < r_snapshot.elements.size(); element_index++) {
		AuthoringElement &authoring = r_snapshot.elements.write[element_index];
		Ref<WFCGraphElement> graph_element = p_graph->get_element(int64_t(authoring.node_id));
		if (graph_element.is_null()) {
			r_error = "WFC graph processor lost an element mapping.";
			return false;
		}

		PackedStringArray allowed_options = graph_element->get_allowed_options();
		HashSet<StringName> unmatched_options;
		for (int option_index = 0; option_index < allowed_options.size(); option_index++) {
			unmatched_options.insert(StringName(allowed_options[option_index]));
		}

		for (int option_index = 0; option_index < authoring.options.size(); option_index++) {
			AuthoringOption &option = authoring.options.write[option_index];
			bool enabled = unmatched_options.has(option.option);
			option.enabled = enabled;
			if (enabled) {
				unmatched_options.erase(option.option);
			}
		}

		if (!unmatched_options.is_empty()) {
			r_error = vformat("WFC graph processor selected unknown options for element type '%s'.", String(authoring.type));
			return false;
		}

		authoring.resolve_priority = graph_element->get_resolve_priority();
		authoring.defer_collapse = graph_element->is_defer_collapse_enabled();
	}

	return true;
}

static void _apply_context_to_graph(const SolveContext &p_context, const Ref<WFCGraph> &p_graph) {
	for (int element_index = 0; element_index < p_context.elements.size(); element_index++) {
		const SolvedElement &element = p_context.elements[element_index];
		const CompiledCatalog::TypeData &type_data = p_context.catalog.types[element.type_id];
		Ref<WFCGraphElement> graph_element = p_graph->get_element(int64_t(element.node_id));
		if (graph_element.is_null()) {
			continue;
		}

		graph_element->set_allowed_options(_export_allowed_options(element, type_data));
		if (!element.defer_collapse && element.remaining == 1) {
			graph_element->set_selected_option(StringName(graph_element->get_allowed_options()[0]));
		} else {
			graph_element->set_selected_option(StringName());
		}
	}
}

static bool _populate_solve_result(const SolveContext &p_context, const Ref<WFCGraph> &p_graph, SolveResult &r_result, String &r_error) {
	r_result.success = true;
	r_result.connection_count = p_context.connection_count;
	r_result.node_ids.resize(p_context.elements.size());
	r_result.selected_options.resize(p_context.elements.size());
	r_result.selected_rotation_steps.resize(p_context.elements.size());
	r_result.remaining_options.resize(p_context.elements.size());
	r_result.persist_remaining_options.resize(p_context.elements.size());
	r_result.resolved_data.resize(p_context.elements.size());

	for (int element_index = 0; element_index < p_context.elements.size(); element_index++) {
		const SolvedElement &element = p_context.elements[element_index];
		const CompiledCatalog::TypeData &type_data = p_context.catalog.types[element.type_id];
		PackedStringArray allowed_options = _export_allowed_options(element, type_data);

		r_result.node_ids.write[element_index] = element.node_id;
		r_result.remaining_options.write[element_index] = allowed_options;
		r_result.persist_remaining_options.write[element_index] = element.defer_collapse;
		r_result.selected_rotation_steps.write[element_index] = 0;

		if (!element.defer_collapse) {
			if (allowed_options.size() != 1) {
				r_result.success = false;
				r_error = vformat("WFC solver failed to collapse element type '%s' to a single option.", String(type_data.name));
				return false;
			}
			r_result.selected_options.write[element_index] = StringName(allowed_options[0]);

			// Find which variant is set and extract rotation step
			int variant_count = type_data.variant_base_option.size();
			for (int vi = 0; vi < variant_count; vi++) {
				if (element.options.has_bit(vi)) {
					r_result.selected_rotation_steps.write[element_index] = type_data.variant_rotation[vi];
					break;
				}
			}
		}

		if (p_graph.is_valid()) {
			Ref<WFCGraphElement> graph_element = p_graph->get_element(int64_t(element.node_id));
			if (graph_element.is_valid()) {
				r_result.resolved_data.write[element_index] = graph_element->get_resolved_data();
			}
		}
	}

	return true;
}

static bool _build_solve_context(const AuthoringSnapshot &p_snapshot, SolveContext &r_context, Vector<ConnectionBuild> *r_connection_preview, String &r_error) {
	// Collect symmetry folds from authoring elements
	HashMap<StringName, HashMap<StringName, int>> symmetry_map;
	for (int element_index = 0; element_index < p_snapshot.elements.size(); element_index++) {
		const AuthoringElement &authoring = p_snapshot.elements[element_index];
		if (!symmetry_map.has(authoring.type)) {
			symmetry_map[authoring.type] = HashMap<StringName, int>();
		}
		HashMap<StringName, int> &type_sym = symmetry_map[authoring.type];
		for (int option_index = 0; option_index < authoring.options.size(); option_index++) {
			const AuthoringOption &option = authoring.options[option_index];
			if (!type_sym.has(option.option)) {
				type_sym[option.option] = option.symmetry_fold;
			} else {
				type_sym[option.option] = MAX(type_sym[option.option], option.symmetry_fold);
			}
		}
	}

	if (!_compile_catalog(p_snapshot.merged_rules, symmetry_map, r_context.catalog, r_error)) {
		return false;
	}

	Vector<ConnectionBuild> connections;
	_build_connections(p_snapshot, connections);
	r_context.connection_count = connections.size();
	if (r_connection_preview != nullptr) {
		*r_connection_preview = connections;
	}

	r_context.elements.resize(p_snapshot.elements.size());
	for (int element_index = 0; element_index < p_snapshot.elements.size(); element_index++) {
		const AuthoringElement &authoring = p_snapshot.elements[element_index];
		if (!r_context.catalog.type_ids.has(authoring.type)) {
			r_error = vformat("Unknown WFC type '%s' on element.", String(authoring.type));
			return false;
		}
		int type_id = r_context.catalog.type_ids[authoring.type];
		const CompiledCatalog::TypeData &type_data = r_context.catalog.types[type_id];

		SolvedElement &element = r_context.elements.write[element_index];
		element.node_id = authoring.node_id;
		element.type_id = type_id;
		element.resolve_priority = authoring.resolve_priority;
		element.defer_collapse = authoring.defer_collapse;
		element.position = authoring.global_transform.origin;
		element.options.resize(type_data.option_word_count);

		int variant_count = type_data.variant_base_option.size();
		element.weights.resize(variant_count);
		for (int weight_index = 0; weight_index < variant_count; weight_index++) {
			element.weights.write[weight_index] = 0.0f;
		}

		HashSet<int> watched_sides;
		for (int neighbor_index = 0; neighbor_index < authoring.neighbors.size(); neighbor_index++) {
			const AuthoringNeighbor &neighbor = authoring.neighbors[neighbor_index];
			if (!type_data.side_ids.has(neighbor.name)) {
				continue;
			}
			int side_id = type_data.side_ids[neighbor.name];
			if (watched_sides.has(side_id)) {
				continue;
			}
			watched_sides.insert(side_id);
			SolvedSide side;
			side.side_id = side_id;
			element.sides.push_back(side);
		}

		for (int option_index = 0; option_index < authoring.options.size(); option_index++) {
			const AuthoringOption &option = authoring.options[option_index];
			if (!option.enabled || !type_data.option_ids.has(option.option)) {
				continue;
			}
			int base_option_id = type_data.option_ids[option.option];

			// Enable all rotation variants of this base option
			for (int vi = 0; vi < variant_count; vi++) {
				if (type_data.variant_base_option[vi] == base_option_id) {
					element.options.set_bit(vi);
					element.weights.write[vi] = option.probability;
				}
			}
		}

		element.remaining = element.options.count_bits();
		if (element.remaining == 0) {
			r_error = vformat("Element of type '%s' has no enabled options that exist in the compiled catalog.", String(authoring.type));
			return false;
		}
	}

	for (int connection_index = 0; connection_index < connections.size(); connection_index++) {
		const ConnectionBuild &connection = connections[connection_index];
		SolvedElement &from_element = r_context.elements.write[connection.from_index];
		SolvedElement &to_element = r_context.elements.write[connection.to_index];
		const CompiledCatalog::TypeData &from_type = r_context.catalog.types[from_element.type_id];
		const CompiledCatalog::TypeData &to_type = r_context.catalog.types[to_element.type_id];
		if (!from_type.side_ids.has(connection.from_side) || !to_type.side_ids.has(connection.to_side)) {
			continue;
		}
		int from_side_id = from_type.side_ids[connection.from_side];
		int to_side_id = to_type.side_ids[connection.to_side];

		for (int side_index = 0; side_index < from_element.sides.size(); side_index++) {
			if (from_element.sides[side_index].side_id == from_side_id) {
				from_element.sides.write[side_index].other_index = connection.to_index;
				from_element.sides.write[side_index].other_side_id = to_side_id;
				break;
			}
		}
		for (int side_index = 0; side_index < to_element.sides.size(); side_index++) {
			if (to_element.sides[side_index].side_id == to_side_id) {
				to_element.sides.write[side_index].other_index = connection.from_index;
				to_element.sides.write[side_index].other_side_id = from_side_id;
				break;
			}
		}
		if (from_element.connected_elements.find(connection.to_index) == -1) {
			from_element.connected_elements.push_back(connection.to_index);
		}
		if (to_element.connected_elements.find(connection.from_index) == -1) {
			to_element.connected_elements.push_back(connection.from_index);
		}
	}

	return true;
}

static void _apply_preview_connections(const AuthoringSnapshot &p_snapshot, const Vector<ConnectionBuild> &p_connections) {
	for (int element_index = 0; element_index < p_snapshot.elements.size(); element_index++) {
		WFCElement *element = Object::cast_to<WFCElement>(ObjectDB::get_instance(p_snapshot.elements[element_index].node_id));
		if (element != nullptr) {
			element->clear_connected_neighbors();
		}
	}

	for (int connection_index = 0; connection_index < p_connections.size(); connection_index++) {
		const ConnectionBuild &connection = p_connections[connection_index];
		WFCElement *from_element = Object::cast_to<WFCElement>(ObjectDB::get_instance(p_snapshot.elements[connection.from_index].node_id));
		WFCElement *to_element = Object::cast_to<WFCElement>(ObjectDB::get_instance(p_snapshot.elements[connection.to_index].node_id));
		if (from_element == nullptr || to_element == nullptr) {
			continue;
		}
		from_element->set_connected_neighbor(connection.from_side, to_element);
		to_element->set_connected_neighbor(connection.to_side, from_element);
	}
}

static int _pick_weighted_option(const SolvedElement &p_element, RandomPCG &p_rng) {
	float total_weight = 0.0f;
	Vector<int> enabled_options;
	Vector<float> enabled_weights;
	for (int option_index = 0; option_index < p_element.weights.size(); option_index++) {
		if (!p_element.options.has_bit(option_index)) {
			continue;
		}
		enabled_options.push_back(option_index);
		float weight = MAX(0.0f, p_element.weights[option_index]);
		enabled_weights.push_back(weight);
		total_weight += weight;
	}

	if (enabled_options.is_empty()) {
		return -1;
	}
	if (enabled_options.size() == 1) {
		return enabled_options[0];
	}
	if (total_weight <= 0.0f) {
		return enabled_options[p_rng.rand(enabled_options.size())];
	}

	float target = p_rng.randf() * total_weight;
	float cumulative = 0.0f;
	for (int i = 0; i < enabled_options.size(); i++) {
		cumulative += enabled_weights[i];
		if (target <= cumulative) {
			return enabled_options[i];
		}
	}

	return enabled_options[enabled_options.size() - 1];
}

static SolveResult _solve_snapshot(const AuthoringSnapshot &p_snapshot, const Ref<WFCGraphProcessor> &p_graph_processor, Vector<ConnectionBuild> *r_connection_preview = nullptr) {
	SolveResult result;
	AuthoringSnapshot working_snapshot = p_snapshot;
	Ref<WFCGraph> graph;
	if (p_graph_processor.is_valid() || r_connection_preview != nullptr) {
		Vector<ConnectionBuild> preview_connections;
		_build_connections(working_snapshot, preview_connections);
		if (r_connection_preview != nullptr) {
			*r_connection_preview = preview_connections;
		}
		if (p_graph_processor.is_valid()) {
			String graph_error;
			if (!_build_graph_snapshot(working_snapshot, preview_connections, graph, graph_error)) {
				result.error = graph_error;
				return result;
			}
			p_graph_processor->pre_resolve(graph);
			if (!_apply_graph_to_snapshot(graph, working_snapshot, graph_error)) {
				result.error = graph_error;
				return result;
			}
		}
	}

	SolveContext context;
	String error;
	if (!_build_solve_context(working_snapshot, context, nullptr, error)) {
		result.error = error;
		return result;
	}

	RandomPCG rng;
	if (working_snapshot.seed != 0) {
		rng.seed(working_snapshot.seed);
	} else {
		rng.randomize();
	}

	Vector<int> queue;
	Vector<bool> in_queue;
	in_queue.resize(context.elements.size());
	for (int i = 0; i < in_queue.size(); i++) {
		in_queue.write[i] = true;
		queue.push_back(i);
	}

	auto enqueue = [&](int p_index) {
		if (!in_queue[p_index]) {
			in_queue.write[p_index] = true;
			queue.push_back(p_index);
		}
	};

	auto process_queue = [&]() -> bool {
		while (!queue.is_empty()) {
			int element_index = queue[queue.size() - 1];
			queue.resize(queue.size() - 1);
			in_queue.write[element_index] = false;
			SolvedElement &element = context.elements.write[element_index];
			const CompiledCatalog::TypeData &type_data = context.catalog.types[element.type_id];

			for (int side_index = 0; side_index < element.sides.size(); side_index++) {
				const SolvedSide &side = element.sides[side_index];
				if (side.other_index >= 0) {
					const SolvedElement &other = context.elements[side.other_index];
					const CompiledCatalog::TypeData &other_type = context.catalog.types[other.type_id];
					BitMask my_connections = _possible_connections(type_data, side.side_id, element.options, context.catalog.connection_word_count);
					BitMask other_connections = _possible_connections(other_type, side.other_side_id, other.options, context.catalog.connection_word_count);
					// Expand other_connections through pair masks: for each connection d
					// the other offers, include all connections c such that c pairs with d
					BitMask expanded_other;
					expanded_other.resize(context.catalog.connection_word_count);
					int conn_count = context.catalog.connection_names.size();
					for (int ci = 0; ci < conn_count; ci++) {
						if (other_connections.has_bit(ci)) {
							expanded_other.bit_or(context.catalog.connection_pair_masks[ci]);
						}
					}
					// My allowed connections = what I offer AND pairs with something the other offers
					BitMask allowed_connections = my_connections.intersected(expanded_other);
					BitMask allowed = _allowed_options(type_data, side.side_id, allowed_connections);
					bool changed = element.options.bit_and_in_place(allowed);
					if (changed) {
						element.remaining = element.options.count_bits();
						if (element.remaining == 0) {
							float cell_size = p_snapshot.cell_size;
							int gx = Math::round(element.position.x / cell_size);
							int gy = Math::round(element.position.y / cell_size);
							int gz = Math::round(element.position.z / cell_size);
							result.error = vformat("Contradiction while solving WFC element of type '%s' at grid(%d,%d).", String(type_data.name), gx, gz);
							return false;
						}
						for (int neighbor_index = 0; neighbor_index < element.connected_elements.size(); neighbor_index++) {
							enqueue(element.connected_elements[neighbor_index]);
						}
					}
				} else {
					// No neighbor: keep options that explicitly list "none", plus options
					// offering connections that pair with "none" per @connection_pairs
					BitMask no_neighbor_mask = type_data.sides[side.side_id].none_options;
					const BitMask &none_pair_mask = context.catalog.connection_pair_masks[context.catalog.none_connection_id];
					BitMask none_pairing_opts = _allowed_options(type_data, side.side_id, none_pair_mask);
					no_neighbor_mask.bit_or(none_pairing_opts);
					bool changed = element.options.bit_and_in_place(no_neighbor_mask);
					if (changed) {
						element.remaining = element.options.count_bits();
						if (element.remaining == 0) {
							float cell_size = p_snapshot.cell_size;
							int gx = Math::round(element.position.x / cell_size);
							int gy = Math::round(element.position.y / cell_size);
							int gz = Math::round(element.position.z / cell_size);
							result.error = vformat("Contradiction (no-neighbor) while solving WFC element of type '%s' at grid(%d,%d).", String(type_data.name), gx, gz);
							return false;
						}
						for (int neighbor_index = 0; neighbor_index < element.connected_elements.size(); neighbor_index++) {
							enqueue(element.connected_elements[neighbor_index]);
						}
					}
				}
			}
		}
		return true;
	};

	if (!process_queue()) {
		return result;
	}

	HashSet<int> priority_set;
	Vector<int> priorities;
	for (int element_index = 0; element_index < context.elements.size(); element_index++) {
		const SolvedElement &element = context.elements[element_index];
		if (element.defer_collapse || priority_set.has(element.resolve_priority)) {
			continue;
		}
		priority_set.insert(element.resolve_priority);
		priorities.push_back(element.resolve_priority);
	}
	if (priorities.size() > 1) {
		SortArray<int> sorter;
		sorter.sort(priorities.ptrw(), priorities.size());
	}

	for (int priority_index = priorities.size() - 1; priority_index >= 0; priority_index--) {
		int current_priority = priorities[priority_index];
		while (true) {
			int min_remaining = INT32_MAX;
			Vector<int> choices;
			for (int element_index = 0; element_index < context.elements.size(); element_index++) {
				const SolvedElement &element = context.elements[element_index];
				if (element.defer_collapse || element.resolve_priority != current_priority || element.remaining <= 1) {
					continue;
				}
				if (element.remaining < min_remaining) {
					min_remaining = element.remaining;
					choices.clear();
				}
				if (element.remaining == min_remaining) {
					choices.push_back(element_index);
				}
			}

			if (choices.is_empty()) {
				break;
			}

			int selected_element_index = choices[rng.rand(choices.size())];
			SolvedElement &selected_element = context.elements.write[selected_element_index];
			int selected_option = _pick_weighted_option(selected_element, rng);
			if (selected_option < 0) {
				result.error = "Failed to select a weighted WFC option.";
				return result;
			}

			BitMask collapsed;
			collapsed.resize(selected_element.options.words.size());
			collapsed.set_bit(selected_option);
			selected_element.options = collapsed;
			selected_element.remaining = 1;
			for (int neighbor_index = 0; neighbor_index < selected_element.connected_elements.size(); neighbor_index++) {
				enqueue(selected_element.connected_elements[neighbor_index]);
			}

			if (!process_queue()) {
				return result;
			}
		}
	}

	if (graph.is_valid()) {
		_apply_context_to_graph(context, graph);
		p_graph_processor->post_resolve(graph);
	}

	if (!_populate_solve_result(context, graph, result, error)) {
		result.error = error;
		return result;
	}
	return result;
}

} // namespace

struct WFCSolver::AsyncJob {
	AuthoringSnapshot snapshot;
	Ref<WFCGraphProcessor> graph_processor;
	Vector<ConnectionBuild> preview_connections;
	bool collect_preview_connections = false;
	SolveResult result;
	bool wait_called = false;
	uint64_t snapshot_build_usec = 0;
	int element_count = 0;
	uint64_t submit_time_usec = 0;
};

void WFCSolver::_solve_async_task(void *p_userdata) {
	AsyncJob *job = static_cast<AsyncJob *>(p_userdata);
	job->result = _solve_snapshot(job->snapshot, job->graph_processor, job->collect_preview_connections ? &job->preview_connections : nullptr);
}

void WFCSolver::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_catalog_set", "catalog_set"), &WFCSolver::set_catalog_set);
	ClassDB::bind_method(D_METHOD("get_catalog_set"), &WFCSolver::get_catalog_set);
	ClassDB::bind_method(D_METHOD("set_graph_processor", "graph_processor"), &WFCSolver::set_graph_processor);
	ClassDB::bind_method(D_METHOD("get_graph_processor"), &WFCSolver::get_graph_processor);
	ClassDB::bind_method(D_METHOD("set_cell_size", "cell_size"), &WFCSolver::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &WFCSolver::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &WFCSolver::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &WFCSolver::get_seed);
	ClassDB::bind_method(D_METHOD("set_auto_materialize", "auto_materialize"), &WFCSolver::set_auto_materialize);
	ClassDB::bind_method(D_METHOD("is_auto_materialize_enabled"), &WFCSolver::is_auto_materialize_enabled);
	ClassDB::bind_method(D_METHOD("get_last_error"), &WFCSolver::get_last_error);
	ClassDB::bind_method(D_METHOD("get_last_resolved_elements"), &WFCSolver::get_last_resolved_elements);
	ClassDB::bind_method(D_METHOD("is_solving"), &WFCSolver::is_solving);
	ClassDB::bind_method(D_METHOD("reset"), &WFCSolver::reset);
	ClassDB::bind_method(D_METHOD("cleanup"), &WFCSolver::cleanup);
	ClassDB::bind_method(D_METHOD("add_element", "element"), &WFCSolver::add_element);
	ClassDB::bind_method(D_METHOD("add_branch", "root"), &WFCSolver::add_branch);
	ClassDB::bind_method(D_METHOD("connect_neighbors"), &WFCSolver::connect_neighbors);
	ClassDB::bind_method(D_METHOD("resolve"), &WFCSolver::resolve);
	ClassDB::bind_method(D_METHOD("resolve_branch_async", "root", "root_global_transform"), &WFCSolver::resolve_branch_async);
	ClassDB::bind_method(D_METHOD("resolve_branch_sync", "root", "root_global_transform"), &WFCSolver::resolve_branch_sync);
	ClassDB::bind_method(D_METHOD("resolve_sync"), &WFCSolver::resolve_sync);
	ClassDB::bind_method(D_METHOD("resolve_async"), &WFCSolver::resolve_async);
	ClassDB::bind_method(D_METHOD("materialize"), &WFCSolver::materialize);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "catalog_set", PROPERTY_HINT_RESOURCE_TYPE, "WFCCatalogSet"), "set_catalog_set", "get_catalog_set");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "graph_processor", PROPERTY_HINT_RESOURCE_TYPE, "WFCGraphProcessor"), "set_graph_processor", "get_graph_processor");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.001,1024,0.001,or_greater"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed", PROPERTY_HINT_RANGE, "0,18446744073709551615,1,or_greater"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_materialize"), "set_auto_materialize", "is_auto_materialize_enabled");

	ADD_SIGNAL(MethodInfo("solve_started"));
	ADD_SIGNAL(MethodInfo("solve_completed", PropertyInfo(Variant::BOOL, "success"), PropertyInfo(Variant::STRING, "error")));
}

void WFCSolver::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS && async_job != nullptr && WorkerThreadPool::get_singleton()->is_task_completed(async_task_id)) {
		_finish_async_job();
	}
}

void WFCSolver::_add_branch_recursive(Node *p_node) {
	WFCElement *element = Object::cast_to<WFCElement>(p_node);
	if (element != nullptr) {
		tracked_elements.insert(element->get_instance_id());
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_add_branch_recursive(p_node->get_child(i));
	}
}

static bool _build_authoring_snapshot(HashSet<ObjectID> &p_tracked_elements, const Ref<WFCCatalogSet> &p_catalog_set, float p_cell_size, uint64_t p_seed, AuthoringSnapshot &r_snapshot, String &r_error) {
	r_snapshot.elements.clear();
	if (p_catalog_set.is_null()) {
		r_error = "WFCSolver requires a WFCCatalogSet resource.";
		return false;
	}

	r_snapshot.merged_rules = p_catalog_set->merge_rules();
	r_snapshot.cell_size = MAX(0.001f, p_cell_size);
	r_snapshot.seed = p_seed;

	Vector<ObjectID> stale_elements;
	Vector<WFCElement *> ordered_elements;
	for (const ObjectID &element_id : p_tracked_elements) {
		WFCElement *element = Object::cast_to<WFCElement>(ObjectDB::get_instance(element_id));
		if (element == nullptr) {
			stale_elements.push_back(element_id);
			continue;
		}
		ordered_elements.push_back(element);
	}

	if (ordered_elements.size() > 1) {
		SortArray<WFCElement *, Node::Comparator> sorter;
		sorter.sort(ordered_elements.ptrw(), ordered_elements.size());
	}

	for (int element_index = 0; element_index < ordered_elements.size(); element_index++) {
		WFCElement *element = ordered_elements[element_index];

		AuthoringElement snapshot_element;
		snapshot_element.node_id = element->get_instance_id();
		snapshot_element.type = element->get_type();
		snapshot_element.global_transform = element->get_global_transform();
		snapshot_element.resolve_priority = element->get_resolve_priority();
		snapshot_element.defer_collapse = element->is_defer_collapse_enabled();

		TypedArray<WFCParam> options = element->get_options();
		for (int i = 0; i < options.size(); i++) {
			Ref<WFCParam> option = options[i];
			if (option.is_null()) {
				continue;
			}
			AuthoringOption snapshot_option;
			snapshot_option.option = option->get_option();
			snapshot_option.probability = option->get_probability();
			snapshot_option.enabled = option->is_enabled();
			snapshot_option.symmetry_fold = option->get_symmetry_fold();
			snapshot_element.options.push_back(snapshot_option);
		}

		TypedArray<WFCNeighbor> neighbors = element->get_neighbor_points();
		for (int i = 0; i < neighbors.size(); i++) {
			Ref<WFCNeighbor> neighbor = neighbors[i];
			if (neighbor.is_null()) {
				continue;
			}
			AuthoringNeighbor snapshot_neighbor;
			snapshot_neighbor.name = neighbor->get_side_name();
			snapshot_neighbor.inv_name = neighbor->get_inv_name();
			snapshot_neighbor.type = neighbor->get_type();
			snapshot_neighbor.offset = neighbor->get_offset();
			snapshot_neighbor.wobble = neighbor->get_wobble();
			snapshot_neighbor.angular_wobble = neighbor->get_angular_wobble();
			snapshot_neighbor.primary = neighbor->is_primary();
			snapshot_neighbor.rotation_lock = neighbor->get_rotation_lock();
			snapshot_element.neighbors.push_back(snapshot_neighbor);
		}

		r_snapshot.elements.push_back(snapshot_element);
	}

	for (int i = 0; i < stale_elements.size(); i++) {
		p_tracked_elements.erase(stale_elements[i]);
	}

	if (r_snapshot.elements.is_empty()) {
		r_error = "WFCSolver has no tracked WFCElement nodes.";
		return false;
	}

	return true;
}

static void _append_authoring_element_snapshot(WFCElement *p_element, const Transform3D &p_global_transform, AuthoringSnapshot &r_snapshot) {
	AuthoringElement snapshot_element;
	snapshot_element.node_id = p_element->get_instance_id();
	snapshot_element.type = p_element->get_type();
	snapshot_element.global_transform = p_global_transform;
	snapshot_element.resolve_priority = p_element->get_resolve_priority();
	snapshot_element.defer_collapse = p_element->is_defer_collapse_enabled();

	TypedArray<WFCParam> options = p_element->get_options();
	for (int i = 0; i < options.size(); i++) {
		Ref<WFCParam> option = options[i];
		if (option.is_null()) {
			continue;
		}
		AuthoringOption snapshot_option;
		snapshot_option.option = option->get_option();
		snapshot_option.probability = option->get_probability();
		snapshot_option.enabled = option->is_enabled();
		snapshot_option.symmetry_fold = option->get_symmetry_fold();
		snapshot_element.options.push_back(snapshot_option);
	}

	TypedArray<WFCNeighbor> neighbors = p_element->get_neighbor_points();
	for (int i = 0; i < neighbors.size(); i++) {
		Ref<WFCNeighbor> neighbor = neighbors[i];
		if (neighbor.is_null()) {
			continue;
		}
		AuthoringNeighbor snapshot_neighbor;
		snapshot_neighbor.name = neighbor->get_side_name();
		snapshot_neighbor.inv_name = neighbor->get_inv_name();
		snapshot_neighbor.type = neighbor->get_type();
		snapshot_neighbor.offset = neighbor->get_offset();
		snapshot_neighbor.wobble = neighbor->get_wobble();
		snapshot_neighbor.angular_wobble = neighbor->get_angular_wobble();
		snapshot_neighbor.primary = neighbor->is_primary();
		snapshot_neighbor.rotation_lock = neighbor->get_rotation_lock();
		snapshot_element.neighbors.push_back(snapshot_neighbor);
	}

	r_snapshot.elements.push_back(snapshot_element);
}

static void _build_authoring_snapshot_for_branch_recursive(Node *p_node, const Transform3D &p_node_global_transform, AuthoringSnapshot &r_snapshot) {
	WFCElement *element = Object::cast_to<WFCElement>(p_node);
	if (element != nullptr) {
		_append_authoring_element_snapshot(element, p_node_global_transform, r_snapshot);
	}

	for (int child_index = 0; child_index < p_node->get_child_count(); child_index++) {
		Node *child = p_node->get_child(child_index);
		Transform3D child_global_transform = p_node_global_transform;
		Node3D *child_3d = Object::cast_to<Node3D>(child);
		if (child_3d != nullptr) {
			child_global_transform = p_node_global_transform * child_3d->get_transform();
		}
		_build_authoring_snapshot_for_branch_recursive(child, child_global_transform, r_snapshot);
	}
}

static bool _build_authoring_snapshot_for_branch(Node3D *p_root, const Transform3D &p_root_global_transform, const Ref<WFCCatalogSet> &p_catalog_set, float p_cell_size, uint64_t p_seed, AuthoringSnapshot &r_snapshot, String &r_error) {
	r_snapshot.elements.clear();
	if (p_root == nullptr) {
		r_error = "WFCSolver requires a valid detached root Node3D.";
		return false;
	}
	if (p_catalog_set.is_null()) {
		r_error = "WFCSolver requires a WFCCatalogSet resource.";
		return false;
	}

	r_snapshot.merged_rules = p_catalog_set->merge_rules();
	r_snapshot.cell_size = MAX(0.001f, p_cell_size);
	r_snapshot.seed = p_seed;
	_build_authoring_snapshot_for_branch_recursive(p_root, p_root_global_transform, r_snapshot);

	if (r_snapshot.elements.is_empty()) {
		r_error = "WFCSolver branch snapshot has no WFCElement nodes.";
		return false;
	}

	return true;
}

static void _apply_solve_result_to_live_elements(const SolveResult &p_result) {
	for (int i = 0; i < p_result.node_ids.size(); i++) {
		WFCElement *element = Object::cast_to<WFCElement>(ObjectDB::get_instance(p_result.node_ids[i]));
		if (element == nullptr) {
			continue;
		}
		Dictionary data = p_result.resolved_data[i];
		int rotation_step = p_result.selected_rotation_steps[i];
		data["wfc_rotation_step"] = rotation_step;

		// Compute rotation angle in degrees from the selected option's symmetry_fold
		float rotation_degrees = 0.0f;
		if (rotation_step > 0 && !p_result.selected_options[i].is_empty()) {
			TypedArray<WFCParam> options = element->get_options();
			for (int opt_idx = 0; opt_idx < options.size(); opt_idx++) {
				Ref<WFCParam> opt = options[opt_idx];
				if (opt.is_valid() && opt->get_option() == p_result.selected_options[i]) {
					int fold = MAX(1, opt->get_symmetry_fold());
					if (fold > 1) {
						rotation_degrees = float(rotation_step) * 360.0f / float(fold);
					}
					break;
				}
			}
		}
		data["wfc_rotation_degrees"] = rotation_degrees;

		element->set_resolved_data(data);
		if (p_result.persist_remaining_options[i]) {
			element->set_selected_option(StringName());
			element->set_enabled_options(p_result.remaining_options[i]);
		} else {
			element->set_selected_option(p_result.selected_options[i]);
		}
	}
}

void WFCSolver::_finish_async_job() {
	if (async_job == nullptr) {
		return;
	}
	if (!async_job->wait_called) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(async_task_id);
		async_job->wait_called = true;
	}
	if (!async_job->preview_connections.is_empty()) {
		_apply_preview_connections(async_job->snapshot, async_job->preview_connections);
	}

	last_error = async_job->result.error;
	if (async_job->result.success) {
		last_resolved_node_ids = async_job->result.node_ids;
		_apply_solve_result_to_live_elements(async_job->result);
		if (auto_materialize) {
			materialize();
		}
	} else {
		last_resolved_node_ids.clear();
	}

	emit_signal(SNAME("solve_completed"), async_job->result.success, async_job->result.error);
	_clear_async_job();
}

void WFCSolver::_clear_async_job() {
	if (async_job == nullptr) {
		return;
	}
	if (!async_job->wait_called && async_task_id != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(async_task_id);
	}
	memdelete(async_job);
	async_job = nullptr;
	async_task_id = WorkerThreadPool::INVALID_TASK_ID;
	set_process(false);
}

void WFCSolver::set_catalog_set(const Ref<WFCCatalogSet> &p_catalog_set) {
	catalog_set = p_catalog_set;
}

Ref<WFCCatalogSet> WFCSolver::get_catalog_set() const {
	return catalog_set;
}

void WFCSolver::set_graph_processor(const Ref<WFCGraphProcessor> &p_graph_processor) {
	graph_processor = p_graph_processor;
}

Ref<WFCGraphProcessor> WFCSolver::get_graph_processor() const {
	return graph_processor;
}

void WFCSolver::set_cell_size(float p_cell_size) {
	cell_size = MAX(0.001f, p_cell_size);
}

float WFCSolver::get_cell_size() const {
	return cell_size;
}

void WFCSolver::set_seed(uint64_t p_seed) {
	seed = p_seed;
}

uint64_t WFCSolver::get_seed() const {
	return seed;
}

void WFCSolver::set_auto_materialize(bool p_auto_materialize) {
	auto_materialize = p_auto_materialize;
}

bool WFCSolver::is_auto_materialize_enabled() const {
	return auto_materialize;
}

String WFCSolver::get_last_error() const {
	return last_error;
}

TypedArray<WFCElement> WFCSolver::get_last_resolved_elements() const {
	TypedArray<WFCElement> elements;
	for (int i = 0; i < last_resolved_node_ids.size(); i++) {
		WFCElement *element = Object::cast_to<WFCElement>(ObjectDB::get_instance(last_resolved_node_ids[i]));
		if (element != nullptr) {
			elements.append(element);
		}
	}
	return elements;
}

bool WFCSolver::is_solving() const {
	return async_job != nullptr;
}

void WFCSolver::reset() {
	tracked_elements.clear();
	last_resolved_node_ids.clear();
	last_error = String();
}

void WFCSolver::cleanup() {
	_clear_async_job();
	tracked_elements.clear();
	last_resolved_node_ids.clear();
}

void WFCSolver::add_element(WFCElement *p_element) {
	ERR_FAIL_NULL(p_element);
	tracked_elements.insert(p_element->get_instance_id());
}

void WFCSolver::add_branch(Node *p_root) {
	ERR_FAIL_NULL(p_root);
	_add_branch_recursive(p_root);
}

int WFCSolver::connect_neighbors() {
	if (tracked_elements.is_empty()) {
		_add_branch_recursive(this);
	}
	AuthoringSnapshot snapshot;
	String error;
	if (!_build_authoring_snapshot(tracked_elements, catalog_set, cell_size, seed, snapshot, error)) {
		last_error = error;
		return 0;
	}
	SolveContext context;
	Vector<ConnectionBuild> preview_connections;
	if (!_build_solve_context(snapshot, context, &preview_connections, error)) {
		last_error = error;
		return 0;
	}
	_apply_preview_connections(snapshot, preview_connections);
	last_error = String();
	return preview_connections.size();
}

bool WFCSolver::resolve() {
	if (async_job != nullptr) {
		last_error = "WFCSolver is already solving asynchronously.";
		return false;
	}
	return resolve_sync();
}

bool WFCSolver::resolve_sync() {
	last_resolved_node_ids.clear();
	if (tracked_elements.is_empty()) {
		_add_branch_recursive(this);
	}

	AuthoringSnapshot snapshot;
	String error;
	if (!_build_authoring_snapshot(tracked_elements, catalog_set, cell_size, seed, snapshot, error)) {
		last_error = error;
		emit_signal(SNAME("solve_completed"), false, last_error);
		return false;
	}
	Vector<ConnectionBuild> preview_connections;

	emit_signal(SNAME("solve_started"));
	SolveResult result = _solve_snapshot(snapshot, graph_processor, &preview_connections);
	_apply_preview_connections(snapshot, preview_connections);
	last_error = result.error;
	if (!result.success) {
		last_resolved_node_ids.clear();
		emit_signal(SNAME("solve_completed"), false, last_error);
		return false;
	}

	_apply_solve_result_to_live_elements(result);
	last_resolved_node_ids = result.node_ids;
	if (auto_materialize) {
		materialize();
	}
	last_error = String();
	emit_signal(SNAME("solve_completed"), true, String());
	return true;
}

bool WFCSolver::resolve_branch_sync(Node3D *p_root, const Transform3D &p_root_global_transform) {
	last_resolved_node_ids.clear();

	AuthoringSnapshot snapshot;
	String error;
	if (!_build_authoring_snapshot_for_branch(p_root, p_root_global_transform, catalog_set, cell_size, seed, snapshot, error)) {
		last_error = error;
		emit_signal(SNAME("solve_completed"), false, last_error);
		return false;
	}

	Vector<ConnectionBuild> preview_connections;
	emit_signal(SNAME("solve_started"));
	SolveResult result = _solve_snapshot(snapshot, graph_processor, &preview_connections);
	_apply_preview_connections(snapshot, preview_connections);
	last_error = result.error;
	if (!result.success) {
		last_resolved_node_ids.clear();
		emit_signal(SNAME("solve_completed"), false, last_error);
		return false;
	}

	_apply_solve_result_to_live_elements(result);
	last_resolved_node_ids = result.node_ids;
	if (auto_materialize) {
		materialize();
	}
	last_error = String();
	emit_signal(SNAME("solve_completed"), true, String());
	return true;
}

Error WFCSolver::resolve_async() {
	if (async_job != nullptr) {
		return ERR_BUSY;
	}
	last_resolved_node_ids.clear();
	if (tracked_elements.is_empty()) {
		_add_branch_recursive(this);
	}

	AuthoringSnapshot snapshot;
	String error;
	if (!_build_authoring_snapshot(tracked_elements, catalog_set, cell_size, seed, snapshot, error)) {
		last_error = error;
		emit_signal(SNAME("solve_completed"), false, last_error);
		return ERR_CANT_CREATE;
	}

	async_job = memnew(AsyncJob);
	async_job->element_count = snapshot.elements.size();
	async_job->snapshot = snapshot;
	async_job->graph_processor = graph_processor;
	async_job->collect_preview_connections = true;
	async_task_id = WorkerThreadPool::get_singleton()->add_native_task(&WFCSolver::_solve_async_task, async_job, true, SNAME("WFCSolver"));
	set_process(true);
	emit_signal(SNAME("solve_started"));
	return OK;
}

Error WFCSolver::resolve_branch_async(Node3D *p_root, const Transform3D &p_root_global_transform) {
	if (async_job != nullptr) {
		return ERR_BUSY;
	}
	last_resolved_node_ids.clear();

	AuthoringSnapshot snapshot;
	String error;
	if (!_build_authoring_snapshot_for_branch(p_root, p_root_global_transform, catalog_set, cell_size, seed, snapshot, error)) {
		last_error = error;
		emit_signal(SNAME("solve_completed"), false, last_error);
		return ERR_CANT_CREATE;
	}

	async_job = memnew(AsyncJob);
	async_job->element_count = snapshot.elements.size();
	async_job->snapshot = snapshot;
	async_job->graph_processor = graph_processor;
	async_job->collect_preview_connections = true;
	async_task_id = WorkerThreadPool::get_singleton()->add_native_task(&WFCSolver::_solve_async_task, async_job, true, SNAME("WFCSolver"));
	set_process(true);
	emit_signal(SNAME("solve_started"));
	return OK;
}

void WFCSolver::materialize() {
	if (async_job != nullptr) {
		last_error = "WFCSolver cannot materialize while an async solve is still in progress.";
		ERR_PRINT(last_error);
		return;
	}
	materialize_pass_id++;

	Vector<WFCElement *> elements;
	for (const ObjectID &element_id : tracked_elements) {
		WFCElement *element = Object::cast_to<WFCElement>(ObjectDB::get_instance(element_id));
		if (element != nullptr) {
			element->set_meta(StringName("wfc_materialize_pass"), int64_t(materialize_pass_id));
			elements.push_back(element);
		}
	}
	for (int i = 0; i < elements.size(); i++) {
		elements[i]->materialize();
	}
	for (int i = 0; i < elements.size(); i++) {
		if (elements[i]->get_selected_option().is_empty()) {
			continue;
		}
		elements[i]->post_materialize();
	}
}

WFCSolver::~WFCSolver() {
	_clear_async_job();
}