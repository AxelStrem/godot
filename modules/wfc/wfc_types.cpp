/**************************************************************************/
/*  wfc_types.cpp                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "wfc_types.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/hash_set.h"

HashMap<String, Ref<PackedScene>> WFCElement::nested_scene_cache;

namespace {

static bool _variant_to_string_name(const Variant &p_value, StringName &r_name) {
	if (p_value.get_type() == Variant::STRING_NAME || p_value.get_type() == Variant::STRING) {
		r_name = StringName(String(p_value));
		return true;
	}
	return false;
}

static void _normalize_packed_scene_ownership(Node *p_node, Node *p_root) {
	if (p_node == p_root) {
		p_node->set_owner(nullptr);
	} else {
		p_node->set_owner(p_root);
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_normalize_packed_scene_ownership(p_node->get_child(i), p_root);
	}
}

static Ref<PackedScene> _pack_wfc_variant(Node *p_node) {
	ERR_FAIL_NULL_V(p_node, Ref<PackedScene>());
	_normalize_packed_scene_ownership(p_node, p_node);

	Ref<PackedScene> packed_scene;
	packed_scene.instantiate();
	Error err = packed_scene->pack(p_node);

	if (err != OK) {
		packed_scene.unref();
	}

	return packed_scene;
}

} // namespace

void WFCCatalog::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_rules", "rules"), &WFCCatalog::set_rules);
	ClassDB::bind_method(D_METHOD("get_rules"), &WFCCatalog::get_rules);

	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "rules"), "set_rules", "get_rules");
}

void WFCCatalog::set_rules(const Dictionary &p_rules) {
	rules = p_rules;
}

Dictionary WFCCatalog::get_rules() const {
	return rules;
}

void WFCCatalogSet::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_catalogs", "catalogs"), &WFCCatalogSet::set_catalogs);
	ClassDB::bind_method(D_METHOD("get_catalogs"), &WFCCatalogSet::get_catalogs);
	ClassDB::bind_method(D_METHOD("merge_rules"), &WFCCatalogSet::merge_rules);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "catalogs", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("WFCCatalog")), "set_catalogs", "get_catalogs");
}

void WFCCatalogSet::set_catalogs(const TypedArray<WFCCatalog> &p_catalogs) {
	catalogs = p_catalogs;
}

TypedArray<WFCCatalog> WFCCatalogSet::get_catalogs() const {
	return catalogs;
}

Dictionary WFCCatalogSet::merge_rules() const {
	Dictionary merged;
	for (int i = 0; i < catalogs.size(); i++) {
		Ref<WFCCatalog> catalog = catalogs[i];
		if (catalog.is_null()) {
			continue;
		}
		Dictionary rules = catalog->get_rules();
		Array types = rules.keys();
		for (int type_index = 0; type_index < types.size(); type_index++) {
			Variant type_variant = types[type_index];
			if (type_variant.get_type() != Variant::STRING_NAME && type_variant.get_type() != Variant::STRING) {
				continue;
			}
			StringName type_name = type_variant;
			Dictionary merged_type = merged.has(type_name) ? Dictionary(merged[type_name]) : Dictionary();
			Dictionary catalog_type = rules[type_name];
			Array sides = catalog_type.keys();
			for (int side_index = 0; side_index < sides.size(); side_index++) {
				Variant side_variant = sides[side_index];
				if (side_variant.get_type() != Variant::STRING_NAME && side_variant.get_type() != Variant::STRING) {
					continue;
				}
				StringName side_name = side_variant;
				Array merged_entries = merged_type.has(side_name) ? Array(merged_type[side_name]) : Array();
				Array catalog_entries = catalog_type[side_name];
				for (int entry_index = 0; entry_index < catalog_entries.size(); entry_index++) {
					merged_entries.append(catalog_entries[entry_index]);
				}
				merged_type[side_name] = merged_entries;
			}
			merged[type_name] = merged_type;
		}
	}
	return merged;
}

void WFCParam::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_option", "option"), &WFCParam::set_option);
	ClassDB::bind_method(D_METHOD("get_option"), &WFCParam::get_option);
	ClassDB::bind_method(D_METHOD("set_probability", "probability"), &WFCParam::set_probability);
	ClassDB::bind_method(D_METHOD("get_probability"), &WFCParam::get_probability);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &WFCParam::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &WFCParam::is_enabled);
	ClassDB::bind_method(D_METHOD("set_scene", "scene"), &WFCParam::set_scene);
	ClassDB::bind_method(D_METHOD("get_scene"), &WFCParam::get_scene);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "option"), "set_option", "get_option");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probability", PROPERTY_HINT_RANGE, "0,1000,0.001,or_greater"), "set_probability", "get_probability");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "scene", PROPERTY_HINT_RESOURCE_TYPE, "PackedScene"), "set_scene", "get_scene");
}

void WFCParam::set_option(const StringName &p_option) {
	option = p_option;
}

StringName WFCParam::get_option() const {
	return option;
}

void WFCParam::set_probability(float p_probability) {
	probability = MAX(0.0f, p_probability);
}

float WFCParam::get_probability() const {
	return probability;
}

void WFCParam::set_enabled(bool p_enabled) {
	enabled = p_enabled;
}

bool WFCParam::is_enabled() const {
	return enabled;
}

void WFCParam::set_scene(const Ref<PackedScene> &p_scene) {
	scene = p_scene;
}

Ref<PackedScene> WFCParam::get_scene() const {
	return scene;
}

void WFCNeighbor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_side_name", "name"), &WFCNeighbor::set_side_name);
	ClassDB::bind_method(D_METHOD("get_side_name"), &WFCNeighbor::get_side_name);
	ClassDB::bind_method(D_METHOD("set_inv_name", "inv_name"), &WFCNeighbor::set_inv_name);
	ClassDB::bind_method(D_METHOD("get_inv_name"), &WFCNeighbor::get_inv_name);
	ClassDB::bind_method(D_METHOD("set_type", "type"), &WFCNeighbor::set_type);
	ClassDB::bind_method(D_METHOD("get_type"), &WFCNeighbor::get_type);
	ClassDB::bind_method(D_METHOD("set_offset", "offset"), &WFCNeighbor::set_offset);
	ClassDB::bind_method(D_METHOD("get_offset"), &WFCNeighbor::get_offset);
	ClassDB::bind_method(D_METHOD("set_wobble", "wobble"), &WFCNeighbor::set_wobble);
	ClassDB::bind_method(D_METHOD("get_wobble"), &WFCNeighbor::get_wobble);
	ClassDB::bind_method(D_METHOD("set_angle", "angle"), &WFCNeighbor::set_angle);
	ClassDB::bind_method(D_METHOD("get_angle"), &WFCNeighbor::get_angle);
	ClassDB::bind_method(D_METHOD("set_angular_wobble", "angular_wobble"), &WFCNeighbor::set_angular_wobble);
	ClassDB::bind_method(D_METHOD("get_angular_wobble"), &WFCNeighbor::get_angular_wobble);
	ClassDB::bind_method(D_METHOD("set_connection", "connection"), &WFCNeighbor::set_connection);
	ClassDB::bind_method(D_METHOD("get_connection"), &WFCNeighbor::get_connection);
	ClassDB::bind_method(D_METHOD("set_primary", "primary"), &WFCNeighbor::set_primary);
	ClassDB::bind_method(D_METHOD("is_primary"), &WFCNeighbor::is_primary);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "name"), "set_side_name", "get_side_name");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "inv_name"), "set_inv_name", "get_inv_name");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "type"), "set_type", "get_type");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "offset"), "set_offset", "get_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wobble", PROPERTY_HINT_RANGE, "0,100,0.0001,or_greater"), "set_wobble", "get_wobble");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "angle"), "set_angle", "get_angle");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "angular_wobble"), "set_angular_wobble", "get_angular_wobble");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "connection"), "set_connection", "get_connection");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "primary"), "set_primary", "is_primary");
}

void WFCNeighbor::set_side_name(const StringName &p_name) {
	name = p_name;
}

StringName WFCNeighbor::get_side_name() const {
	return name;
}

void WFCNeighbor::set_inv_name(const StringName &p_name) {
	inv_name = p_name;
}

StringName WFCNeighbor::get_inv_name() const {
	return inv_name;
}

void WFCNeighbor::set_type(const StringName &p_type) {
	type = p_type;
}

StringName WFCNeighbor::get_type() const {
	return type;
}

void WFCNeighbor::set_offset(const Vector3 &p_offset) {
	offset = p_offset;
}

Vector3 WFCNeighbor::get_offset() const {
	return offset;
}

void WFCNeighbor::set_wobble(float p_wobble) {
	wobble = MAX(0.0f, p_wobble);
}

float WFCNeighbor::get_wobble() const {
	return wobble;
}

void WFCNeighbor::set_angle(const Vector3 &p_angle) {
	angle = p_angle;
}

Vector3 WFCNeighbor::get_angle() const {
	return angle;
}

void WFCNeighbor::set_angular_wobble(const Vector3 &p_wobble) {
	angular_wobble = p_wobble;
}

Vector3 WFCNeighbor::get_angular_wobble() const {
	return angular_wobble;
}

void WFCNeighbor::set_connection(const StringName &p_connection) {
	connection = p_connection;
}

StringName WFCNeighbor::get_connection() const {
	return connection;
}

void WFCNeighbor::set_primary(bool p_primary) {
	primary = p_primary;
}

bool WFCNeighbor::is_primary() const {
	return primary;
}

void WFCElement::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_type", "type"), &WFCElement::set_type);
	ClassDB::bind_method(D_METHOD("get_type"), &WFCElement::get_type);
	ClassDB::bind_method(D_METHOD("set_options", "options"), &WFCElement::set_options);
	ClassDB::bind_method(D_METHOD("get_options"), &WFCElement::get_options);
	ClassDB::bind_method(D_METHOD("set_neighbor_points", "neighbor_points"), &WFCElement::set_neighbor_points);
	ClassDB::bind_method(D_METHOD("get_neighbor_points"), &WFCElement::get_neighbor_points);
	ClassDB::bind_method(D_METHOD("set_selected_option", "selected_option"), &WFCElement::set_selected_option);
	ClassDB::bind_method(D_METHOD("get_selected_option"), &WFCElement::get_selected_option);
	ClassDB::bind_method(D_METHOD("set_resolve_priority", "resolve_priority"), &WFCElement::set_resolve_priority);
	ClassDB::bind_method(D_METHOD("get_resolve_priority"), &WFCElement::get_resolve_priority);
	ClassDB::bind_method(D_METHOD("set_defer_collapse", "defer_collapse"), &WFCElement::set_defer_collapse);
	ClassDB::bind_method(D_METHOD("is_defer_collapse_enabled"), &WFCElement::is_defer_collapse_enabled);
	ClassDB::bind_method(D_METHOD("set_resolved_data", "resolved_data"), &WFCElement::set_resolved_data);
	ClassDB::bind_method(D_METHOD("get_resolved_data"), &WFCElement::get_resolved_data);
	ClassDB::bind_method(D_METHOD("get_enabled_options"), &WFCElement::get_enabled_options);
	ClassDB::bind_method(D_METHOD("set_enabled_options", "enabled_options"), &WFCElement::set_enabled_options);
	ClassDB::bind_method(D_METHOD("has_connected_neighbor", "side_name"), &WFCElement::has_connected_neighbor);
	ClassDB::bind_method(D_METHOD("get_connected_neighbor", "side_name"), &WFCElement::get_connected_neighbor);
	ClassDB::bind_method(D_METHOD("apply_selected_option", "option"), &WFCElement::apply_selected_option);
	ClassDB::bind_method(D_METHOD("clear_materialized"), &WFCElement::clear_materialized);
	ClassDB::bind_method(D_METHOD("materialize"), &WFCElement::materialize);
	ClassDB::bind_method(D_METHOD("post_materialize"), &WFCElement::post_materialize);
	GDVIRTUAL_BIND(_post_materialize);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "type"), "set_type", "get_type");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "options", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("WFCParam")), "set_options", "get_options");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "neighbor_points", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("WFCNeighbor")), "set_neighbor_points", "get_neighbor_points");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolve_priority", PROPERTY_HINT_RANGE, "-1024,1024,1,or_greater,or_less"), "set_resolve_priority", "get_resolve_priority");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "defer_collapse"), "set_defer_collapse", "is_defer_collapse_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "selected_option", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_INTERNAL), "set_selected_option", "get_selected_option");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "resolved_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_INTERNAL), "set_resolved_data", "get_resolved_data");
}

void WFCElement::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		if (Engine::get_singleton()->is_editor_hint()) {
			return;
		}
		if (nested_scenes.is_empty()) {
			_capture_nested_scenes();
		}
	}
}

void WFCElement::_capture_nested_scenes() {
	uint64_t capture_begin = OS::get_singleton()->get_ticks_usec();
	Vector<Node *> nodes_to_remove;
	int newly_packed = 0;
	for (int i = 0; i < get_child_count(); i++) {
		Node *child = get_child(i);
		if (child == nullptr || !child->has_meta("wfc")) {
			continue;
		}

		Variant meta_value = child->get_meta("wfc");
		if (meta_value.get_type() != Variant::STRING_NAME && meta_value.get_type() != Variant::STRING) {
			continue;
		}

		StringName scene_name = meta_value;
		String cache_key = String(type) + "/" + String(scene_name);
		Ref<PackedScene> packed_scene;
		if (nested_scene_cache.has(cache_key)) {
			packed_scene = nested_scene_cache[cache_key];
		} else {
			packed_scene = _pack_wfc_variant(child);
			if (packed_scene.is_valid()) {
				nested_scene_cache.insert(cache_key, packed_scene);
				newly_packed++;
			}
		}
		if (packed_scene.is_valid()) {
			nested_scenes.insert(scene_name, packed_scene);
			nodes_to_remove.push_back(child);
		}
	}

	for (int i = 0; i < nodes_to_remove.size(); i++) {
		Node *node = nodes_to_remove[i];
		remove_child(node);
		node->queue_free();
	}

	if (newly_packed > 0) {
		uint64_t elapsed_usec = OS::get_singleton()->get_ticks_usec() - capture_begin;
		print_line(vformat("WFC fallback capture: type=%s, variants=%d, time=%.2f ms", String(type), newly_packed, double(elapsed_usec) / 1000.0));
	}
}

void WFCElement::set_type(const StringName &p_type) {
	type = p_type;
}

StringName WFCElement::get_type() const {
	return type;
}

void WFCElement::set_options(const Array &p_options) {
	options.clear();
	for (int i = 0; i < p_options.size(); i++) {
		Ref<WFCParam> option = p_options[i];
		if (option.is_valid()) {
			options.append(option);
		}
	}
}

TypedArray<WFCParam> WFCElement::get_options() const {
	return options;
}

void WFCElement::set_neighbor_points(const Array &p_neighbor_points) {
	neighbor_points.clear();
	for (int i = 0; i < p_neighbor_points.size(); i++) {
		Ref<WFCNeighbor> neighbor = p_neighbor_points[i];
		if (neighbor.is_valid()) {
			neighbor_points.append(neighbor);
		}
	}
}

TypedArray<WFCNeighbor> WFCElement::get_neighbor_points() const {
	return neighbor_points;
}

void WFCElement::set_selected_option(const StringName &p_selected_option) {
	selected_option = p_selected_option;
}

StringName WFCElement::get_selected_option() const {
	return selected_option;
}

void WFCElement::set_resolve_priority(int p_resolve_priority) {
	resolve_priority = p_resolve_priority;
}

int WFCElement::get_resolve_priority() const {
	return resolve_priority;
}

void WFCElement::set_defer_collapse(bool p_defer_collapse) {
	defer_collapse = p_defer_collapse;
}

bool WFCElement::is_defer_collapse_enabled() const {
	return defer_collapse;
}

void WFCElement::set_resolved_data(const Dictionary &p_resolved_data) {
	resolved_data = p_resolved_data;
}

Dictionary WFCElement::get_resolved_data() const {
	return resolved_data;
}

PackedStringArray WFCElement::get_enabled_options() const {
	PackedStringArray enabled_options;
	for (int i = 0; i < options.size(); i++) {
		Ref<WFCParam> option_data = options[i];
		if (option_data.is_valid() && option_data->is_enabled()) {
			enabled_options.push_back(option_data->get_option());
		}
	}
	return enabled_options;
}

void WFCElement::set_enabled_options(const PackedStringArray &p_enabled_options) {
	HashSet<StringName> enabled_set;
	for (int i = 0; i < p_enabled_options.size(); i++) {
		enabled_set.insert(StringName(p_enabled_options[i]));
	}
	bool selected_still_enabled = selected_option.is_empty();
	for (int i = 0; i < options.size(); i++) {
		Ref<WFCParam> option_data = options[i];
		if (!option_data.is_valid()) {
			continue;
		}
		bool enabled = enabled_set.has(option_data->get_option());
		option_data->set_enabled(enabled);
		if (enabled && option_data->get_option() == selected_option) {
			selected_still_enabled = true;
		}
	}
	if (!selected_still_enabled) {
		selected_option = StringName();
	}
}

void WFCElement::clear_connected_neighbors() {
	connected_neighbors.clear();
}

void WFCElement::set_connected_neighbor(const StringName &p_side_name, WFCElement *p_element) {
	if (p_element == nullptr) {
		connected_neighbors.erase(p_side_name);
		return;
	}
	connected_neighbors.insert(p_side_name, p_element->get_instance_id());
}

bool WFCElement::has_connected_neighbor(const StringName &p_side_name) const {
	return connected_neighbors.has(p_side_name) && ObjectDB::get_instance(connected_neighbors[p_side_name]) != nullptr;
}

WFCElement *WFCElement::get_connected_neighbor(const StringName &p_side_name) const {
	if (!connected_neighbors.has(p_side_name)) {
		return nullptr;
	}
	return Object::cast_to<WFCElement>(ObjectDB::get_instance(connected_neighbors[p_side_name]));
}

bool WFCElement::apply_selected_option(const StringName &p_option) {
	for (int i = 0; i < options.size(); i++) {
		Ref<WFCParam> option_data = options[i];
		if (option_data.is_valid() && option_data->get_option() == p_option) {
			selected_option = p_option;
			return true;
		}
	}
	return false;
}

void WFCElement::clear_materialized() {
	Vector<ObjectID> stale_children;
	for (int i = 0; i < materialized_children.size(); i++) {
		Node *child = Object::cast_to<Node>(ObjectDB::get_instance(materialized_children[i]));
		if (child == nullptr) {
			stale_children.push_back(materialized_children[i]);
			continue;
		}
		remove_child(child);
		child->queue_free();
		stale_children.push_back(materialized_children[i]);
	}
	for (int i = 0; i < stale_children.size(); i++) {
		materialized_children.erase(stale_children[i]);
	}
}

void WFCElement::materialize() {
	clear_materialized();
	if (nested_scenes.is_empty()) {
		for (int i = 0; i < get_child_count(); i++) {
			Node *child = get_child(i);
			if (child != nullptr && child->has_meta("wfc")) {
				_capture_nested_scenes();
				break;
			}
		}
	}
	if (selected_option.is_empty()) {
		return;
	}

	for (int i = 0; i < options.size(); i++) {
		Ref<WFCParam> option_data = options[i];
		if (!option_data.is_valid() || option_data->get_option() != selected_option) {
			continue;
		}
		if (option_data->get_scene().is_valid()) {
			Node *node = option_data->get_scene()->instantiate();
			add_child(node);
			materialized_children.push_back(node->get_instance_id());
		}
		break;
	}

	if (nested_scenes.has(selected_option) && nested_scenes[selected_option].is_valid()) {
		Node *node = nested_scenes[selected_option]->instantiate();
		add_child(node);
		materialized_children.push_back(node->get_instance_id());
	}
}

void WFCElement::post_materialize() {
	GDVIRTUAL_CALL(_post_materialize);
}