/**************************************************************************/
/*  wfc_graph.cpp                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "wfc_graph.h"

#include "core/object/class_db.h"
#include "core/templates/hash_set.h"

void WFCGraphElement::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_id"), &WFCGraphElement::get_id);
	ClassDB::bind_method(D_METHOD("get_type"), &WFCGraphElement::get_type);
	ClassDB::bind_method(D_METHOD("get_global_transform"), &WFCGraphElement::get_global_transform);
	ClassDB::bind_method(D_METHOD("set_allowed_options", "allowed_options"), &WFCGraphElement::set_allowed_options);
	ClassDB::bind_method(D_METHOD("get_allowed_options"), &WFCGraphElement::get_allowed_options);
	ClassDB::bind_method(D_METHOD("has_option", "option"), &WFCGraphElement::has_option);
	ClassDB::bind_method(D_METHOD("disable_option", "option"), &WFCGraphElement::disable_option);
	ClassDB::bind_method(D_METHOD("keep_only_options", "options"), &WFCGraphElement::keep_only_options);
	ClassDB::bind_method(D_METHOD("set_selected_option", "selected_option"), &WFCGraphElement::set_selected_option);
	ClassDB::bind_method(D_METHOD("get_selected_option"), &WFCGraphElement::get_selected_option);
	ClassDB::bind_method(D_METHOD("get_neighbors"), &WFCGraphElement::get_neighbors);
	ClassDB::bind_method(D_METHOD("has_neighbor", "side_name"), &WFCGraphElement::has_neighbor);
	ClassDB::bind_method(D_METHOD("get_neighbor_id", "side_name"), &WFCGraphElement::get_neighbor_id);
	ClassDB::bind_method(D_METHOD("set_resolve_priority", "resolve_priority"), &WFCGraphElement::set_resolve_priority);
	ClassDB::bind_method(D_METHOD("get_resolve_priority"), &WFCGraphElement::get_resolve_priority);
	ClassDB::bind_method(D_METHOD("set_defer_collapse", "defer_collapse"), &WFCGraphElement::set_defer_collapse);
	ClassDB::bind_method(D_METHOD("is_defer_collapse_enabled"), &WFCGraphElement::is_defer_collapse_enabled);
	ClassDB::bind_method(D_METHOD("set_resolved_data", "resolved_data"), &WFCGraphElement::set_resolved_data);
	ClassDB::bind_method(D_METHOD("get_resolved_data"), &WFCGraphElement::get_resolved_data);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "allowed_options"), "set_allowed_options", "get_allowed_options");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "selected_option"), "set_selected_option", "get_selected_option");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolve_priority"), "set_resolve_priority", "get_resolve_priority");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "defer_collapse"), "set_defer_collapse", "is_defer_collapse_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "resolved_data"), "set_resolved_data", "get_resolved_data");
}

void WFCGraphElement::_sync_selected_option() {
	if (allowed_options.size() == 1) {
		selected_option = StringName(allowed_options[0]);
		return;
	}
	if (selected_option.is_empty()) {
		return;
	}
	for (int i = 0; i < allowed_options.size(); i++) {
		if (StringName(allowed_options[i]) == selected_option) {
			return;
		}
	}
	selected_option = StringName();
}

void WFCGraphElement::set_id(int64_t p_id) {
	id = p_id;
}

int64_t WFCGraphElement::get_id() const {
	return id;
}

void WFCGraphElement::set_type(const StringName &p_type) {
	type = p_type;
}

StringName WFCGraphElement::get_type() const {
	return type;
}

void WFCGraphElement::set_global_transform(const Transform3D &p_transform) {
	global_transform = p_transform;
}

Transform3D WFCGraphElement::get_global_transform() const {
	return global_transform;
}

void WFCGraphElement::set_allowed_options(const PackedStringArray &p_options) {
	allowed_options = p_options;
	_sync_selected_option();
}

PackedStringArray WFCGraphElement::get_allowed_options() const {
	return allowed_options;
}

bool WFCGraphElement::has_option(const StringName &p_option) const {
	for (int i = 0; i < allowed_options.size(); i++) {
		if (StringName(allowed_options[i]) == p_option) {
			return true;
		}
	}
	return false;
}

void WFCGraphElement::disable_option(const StringName &p_option) {
	PackedStringArray filtered;
	for (int i = 0; i < allowed_options.size(); i++) {
		if (StringName(allowed_options[i]) != p_option) {
			filtered.push_back(allowed_options[i]);
		}
	}
	allowed_options = filtered;
	_sync_selected_option();
}

void WFCGraphElement::keep_only_options(const PackedStringArray &p_options) {
	HashSet<StringName> allowed_set;
	for (int i = 0; i < p_options.size(); i++) {
		allowed_set.insert(StringName(p_options[i]));
	}
	PackedStringArray filtered;
	for (int i = 0; i < allowed_options.size(); i++) {
		if (allowed_set.has(StringName(allowed_options[i]))) {
			filtered.push_back(allowed_options[i]);
		}
	}
	allowed_options = filtered;
	_sync_selected_option();
}

void WFCGraphElement::set_selected_option(const StringName &p_option) {
	selected_option = p_option;
	if (!selected_option.is_empty()) {
		allowed_options.clear();
		allowed_options.push_back(String(selected_option));
	}
}

StringName WFCGraphElement::get_selected_option() const {
	return selected_option;
}

void WFCGraphElement::set_neighbors(const Dictionary &p_neighbors) {
	neighbors = p_neighbors;
}

Dictionary WFCGraphElement::get_neighbors() const {
	return neighbors;
}

void WFCGraphElement::set_neighbor_id(const StringName &p_side_name, int64_t p_neighbor_id) {
	neighbors[p_side_name] = p_neighbor_id;
}

bool WFCGraphElement::has_neighbor(const StringName &p_side_name) const {
	return neighbors.has(p_side_name);
}

int64_t WFCGraphElement::get_neighbor_id(const StringName &p_side_name) const {
	if (!neighbors.has(p_side_name)) {
		return -1;
	}
	return int64_t(neighbors[p_side_name]);
}

void WFCGraphElement::set_resolve_priority(int p_priority) {
	resolve_priority = p_priority;
}

int WFCGraphElement::get_resolve_priority() const {
	return resolve_priority;
}

void WFCGraphElement::set_defer_collapse(bool p_defer_collapse) {
	defer_collapse = p_defer_collapse;
}

bool WFCGraphElement::is_defer_collapse_enabled() const {
	return defer_collapse;
}

void WFCGraphElement::set_resolved_data(const Dictionary &p_resolved_data) {
	resolved_data = p_resolved_data;
}

Dictionary WFCGraphElement::get_resolved_data() const {
	return resolved_data;
}

void WFCGraph::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &WFCGraph::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &WFCGraph::get_seed);
	ClassDB::bind_method(D_METHOD("get_elements"), &WFCGraph::get_elements);
	ClassDB::bind_method(D_METHOD("get_elements_of_type", "type"), &WFCGraph::get_elements_of_type);
	ClassDB::bind_method(D_METHOD("get_element", "id"), &WFCGraph::get_element);
	ClassDB::bind_method(D_METHOD("get_element_count"), &WFCGraph::get_element_count);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed", PROPERTY_HINT_RANGE, "0,18446744073709551615,1,or_greater"), "set_seed", "get_seed");
}

void WFCGraph::set_seed(uint64_t p_seed) {
	seed = p_seed;
}

uint64_t WFCGraph::get_seed() const {
	return seed;
}

Array WFCGraph::get_elements() const {
	return elements;
}

Array WFCGraph::get_elements_of_type(const StringName &p_type) const {
	Array filtered;
	for (int i = 0; i < elements.size(); i++) {
		Ref<WFCGraphElement> element = elements[i];
		if (element.is_valid() && element->get_type() == p_type) {
			filtered.push_back(element);
		}
	}
	return filtered;
}

Ref<WFCGraphElement> WFCGraph::get_element(int64_t p_id) const {
	if (!elements_by_id.has(p_id)) {
		return Ref<WFCGraphElement>();
	}
	return elements_by_id[p_id];
}

int WFCGraph::get_element_count() const {
	return elements.size();
}

void WFCGraph::clear() {
	elements.clear();
	elements_by_id.clear();
}

void WFCGraph::add_element(const Ref<WFCGraphElement> &p_element) {
	if (p_element.is_null()) {
		return;
	}
	elements.push_back(p_element);
	elements_by_id.insert(p_element->get_id(), p_element);
}

void WFCGraphProcessor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("pre_resolve", "graph"), &WFCGraphProcessor::pre_resolve);
	ClassDB::bind_method(D_METHOD("post_resolve", "graph"), &WFCGraphProcessor::post_resolve);
	GDVIRTUAL_BIND(_pre_resolve, "graph");
	GDVIRTUAL_BIND(_post_resolve, "graph");
}

void WFCGraphProcessor::pre_resolve(const Ref<WFCGraph> &p_graph) {
	GDVIRTUAL_CALL(_pre_resolve, p_graph);
}

void WFCGraphProcessor::post_resolve(const Ref<WFCGraph> &p_graph) {
	GDVIRTUAL_CALL(_post_resolve, p_graph);
}