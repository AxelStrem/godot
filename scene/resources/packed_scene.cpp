/**************************************************************************/
/*  packed_scene.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "packed_scene.h"

#include "core/config/engine.h"
#include "core/io/file_access.h"
#include "core/io/missing_resource.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"
#include "core/variant/callable_bind.h"
#include "core/variant/container_type_validate.h"
#include "scene/2d/node_2d.h"
#include "scene/gui/control.h"
#include "scene/main/instance_placeholder.h"
#include "scene/main/missing_node.h"
#include "scene/property_utils.h"

#ifndef _3D_DISABLED
#include "scene/3d/node_3d.h"
#endif // _3D_DISABLED

#define PACKED_SCENE_VERSION 3

#ifdef TOOLS_ENABLED
SceneState::InstantiationWarningNotify SceneState::instantiation_warn_notify = nullptr;
#endif

bool SceneState::can_instantiate() const {
	return nodes.size() > 0;
}

static Array _sanitize_node_pinned_properties(Node *p_node) {
	Array pinned = p_node->get_meta("_edit_pinned_properties_", Array());
	if (pinned.is_empty()) {
		return Array();
	}
	HashSet<StringName> storable_properties;
	p_node->get_storable_properties(storable_properties);
	int i = 0;
	do {
		if (storable_properties.has(pinned[i])) {
			i++;
		} else {
			pinned.remove_at(i);
		}
	} while (i < pinned.size());
	if (pinned.is_empty()) {
		p_node->remove_meta("_edit_pinned_properties_");
	}
	return pinned;
}

static Variant _get_storable_property_base_value(Node *p_node, const StringName &p_name) {
	Variant value = p_node->get_base_value(p_name);

#ifndef _3D_DISABLED
	if (p_name == SNAME("transform")) {
		Node3D *node_3d = Object::cast_to<Node3D>(p_node);
		if (node_3d != nullptr && value == p_node->get(p_name)) {
			const Variant base_position = p_node->get_base_value(SNAME("position"));
			const bool has_base_position = base_position != p_node->get(SNAME("position"));
			const Variant base_basis = p_node->get_base_value(SNAME("basis"));
			const bool has_base_basis = base_basis != p_node->get(SNAME("basis"));
			const Variant base_quaternion = p_node->get_base_value(SNAME("quaternion"));
			const bool has_base_quaternion = base_quaternion != p_node->get(SNAME("quaternion"));
			const Variant base_rotation = p_node->get_base_value(SNAME("rotation"));
			const bool has_base_rotation = base_rotation != p_node->get(SNAME("rotation"));
			const Variant base_scale = p_node->get_base_value(SNAME("scale"));
			const bool has_base_scale = base_scale != p_node->get(SNAME("scale"));

			if (has_base_position || has_base_basis || has_base_quaternion || has_base_rotation || has_base_scale) {
				Node3D base_node_3d;
				base_node_3d.set_rotation_order(EulerOrder(int32_t(p_node->get_base_value(SNAME("rotation_order")))));
				base_node_3d.set_transform(Transform3D(value));

				if (has_base_basis) {
					base_node_3d.set_basis(Basis(base_basis));
				} else if (has_base_quaternion) {
					base_node_3d.set_quaternion(Quaternion(base_quaternion));
					if (has_base_scale) {
						base_node_3d.set_scale(Vector3(base_scale));
					}
				} else {
					if (has_base_rotation) {
						base_node_3d.set_rotation(Vector3(base_rotation));
					}
					if (has_base_scale) {
						base_node_3d.set_scale(Vector3(base_scale));
					}
				}

				if (has_base_position) {
					base_node_3d.set_position(Vector3(base_position));
				}

				value = base_node_3d.get_transform();
			}
		}
	}
#endif

	return value;
Variant SceneState::_duplicate_recursive(const Variant &p_variant, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_remap_cache, const Variant &p_fallback, Node *p_for_scene) {
	switch (p_variant.get_type()) {
		case Variant::OBJECT: {
			// The local-to-scene subresource instance is preserved, thus maintaining the previous sharing relationship.
			// This is mainly used when the sub-scene root is reset in the main scene.
			Ref<Resource> sub_res_of_from = p_variant;
			if (sub_res_of_from.is_valid() && sub_res_of_from->is_local_to_scene()) {
				return get_remap_resource(sub_res_of_from, p_remap_cache, p_fallback, p_for_scene);
			}
		} break;
		case Variant::ARRAY: {
			const Array &src = p_variant;
			const Array &fallback = p_fallback;

			bool has_fallback = true;
			Array dst;
			if (src.is_typed()) {
				const ContainerType &scr_type = src.get_element_type();
				dst.set_typed(scr_type);
				has_fallback = false;
				if (fallback.is_typed()) {
					const ContainerType &fallback_type = fallback.get_element_type();
					has_fallback =
							scr_type.builtin_type == fallback_type.builtin_type &&
							scr_type.class_name == fallback_type.class_name &&
							scr_type.script == fallback_type.script;
				}
			}
			dst.resize(src.size());
			for (int i = 0; i < src.size(); i++) {
				Variant ele_fallback;
				if (has_fallback && fallback.size() > i) {
					ele_fallback = fallback[i];
				}
				dst[i] = _duplicate_recursive(src[i], p_remap_cache, ele_fallback, p_for_scene);
			}
			return dst;
		} break;
		case Variant::DICTIONARY: {
			const Dictionary &src = p_variant;
			const Dictionary &fallback = p_fallback;

			bool has_fallback = true;
			Dictionary dst;
			if (src.is_typed()) {
				dst.set_typed(src.get_typed_key_builtin(), src.get_typed_key_class_name(), src.get_typed_key_script(), src.get_typed_value_builtin(), src.get_typed_value_class_name(), src.get_typed_value_script());
				has_fallback = false;
				if (fallback.is_typed()) {
					has_fallback =
							src.get_typed_key_builtin() == fallback.get_typed_key_builtin() &&
							src.get_typed_key_class_name() == fallback.get_typed_key_class_name() &&
							src.get_typed_key_script() == fallback.get_typed_key_script() &&
							src.get_typed_value_builtin() == fallback.get_typed_value_builtin() &&
							src.get_typed_value_class_name() == fallback.get_typed_value_class_name() &&
							src.get_typed_value_script() == fallback.get_typed_value_script();
				}
			}

			for (const KeyValue<Variant, Variant> &KV : src) {
				const Variant &k = KV.key;
				const Variant &v = KV.value;
				Variant val_fallback;
				// FIXME: as both `src` and `fallback` are remapped values, so if the local-to-scene
				// resource is used as the key, it is difficult to find its fallback value.
				if (has_fallback && fallback.has(k)) {
					val_fallback = fallback[k];
				}
				dst.set(
						_duplicate_recursive(k, p_remap_cache, Variant(), p_for_scene),
						_duplicate_recursive(v, p_remap_cache, val_fallback, p_for_scene));
			}
			return dst;
		} break;
		default: {
		}
	}
	return p_variant;
}

Ref<Resource> SceneState::get_remap_resource(const Ref<Resource> &p_resource, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &remap_cache, const Ref<Resource> &p_fallback, Node *p_for_scene) {
	ERR_FAIL_COND_V(p_resource.is_null(), Ref<Resource>());

	// Find the shared copy of the source resource.
	HashMap<Ref<Resource>, Ref<Resource>>::Iterator R = remap_cache[p_for_scene].find(p_resource);
	if (R) {
		return R->value;
	}

	bool reuse_fallback = p_fallback.is_valid() && p_fallback->is_local_to_scene() && p_fallback->get_class_name() == p_resource->get_class_name();

	if (reuse_fallback) {
		// The fallback resource can only be mapped at most once when it is valid.
		for (const KeyValue<Ref<Resource>, Ref<Resource>> &E : remap_cache[p_for_scene]) {
			if (E.value == p_fallback) {
				reuse_fallback = false;
				break;
			}
		}
	}

	if (reuse_fallback) { // Simply copy the data from the source resource to update the fallback resource that was previously set.
		p_fallback->reset_state(); // May want to reset state.

		List<PropertyInfo> pi;
		p_resource->get_property_list(&pi);
		for (const PropertyInfo &E : pi) {
			if (!(E.usage & PROPERTY_USAGE_STORAGE)) {
				continue;
			}
			if (E.name == "resource_path") {
				continue; // Do not change path.
			}

			Variant value = _duplicate_recursive(p_resource->get(E.name), remap_cache, p_fallback->get(E.name), p_for_scene);

			p_fallback->set(E.name, value);
		}
		remap_cache[p_for_scene][p_resource] = p_fallback;
		return p_fallback;
	}

	// A copy of the source resource is required to overwrite the previous one.
	Ref<Resource> local_dupe = p_resource->duplicate_for_local_scene(p_for_scene, remap_cache[p_for_scene]);
	remap_cache[p_for_scene][p_resource] = local_dupe;
	return local_dupe;
}

static Node *_find_node_by_id(Node *p_owner, Node *p_node, int32_t p_id) {
	if (p_owner == p_node || p_node->get_owner() == p_owner) {
		if (p_node->get_unique_scene_id() == p_id) {
			return p_node;
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *found = _find_node_by_id(p_owner, p_node->get_child(i), p_id);
		if (found) {
			return found;
		}
	}

	return nullptr;
}

static bool _should_skip_foreign_scene_subtree(const Node *p_owner, const Node *p_node) {
	if (!p_node || p_node == p_owner) {
		return false;
	}

	const Node *node_owner = p_node->get_owner();
	if (node_owner == p_owner || p_owner->is_editable_instance(node_owner)) {
		return false;
	}

	return !Node::_has_exposed_descendant_for_owner(p_node, p_owner);
}

SceneInstantiationPlan::SceneInstantiationPlan() {
}

const SceneInstantiationPlan::PlanNodeData *SceneInstantiationPlan::_get_node_data(int p_plan_id) const {
	ERR_FAIL_INDEX_V(p_plan_id, plan_nodes.size(), nullptr);
	return &plan_nodes[p_plan_id];
}

SceneInstantiationPlan::PlanNodeData *SceneInstantiationPlan::_get_node_data_w(int p_plan_id) {
	ERR_FAIL_INDEX_V(p_plan_id, plan_nodes.size(), nullptr);
	return &plan_nodes.write[p_plan_id];
}

void SceneInstantiationPlan::_index_source_path(int p_plan_id, const NodePath &p_source_path) {
	source_path_index[p_source_path].insert(p_plan_id);
}

void SceneInstantiationPlan::_unindex_source_path(int p_plan_id, const NodePath &p_source_path) {
	HashSet<int> *plan_ids = source_path_index.getptr(p_source_path);
	if (plan_ids == nullptr) {
		return;
	}

	plan_ids->erase(p_plan_id);
	if (plan_ids->size() == 0) {
		source_path_index.erase(p_source_path);
	}
}

int SceneInstantiationPlan::_find_node_by_source_path(const NodePath &p_source_path) const {
	const HashSet<int> *plan_ids = source_path_index.getptr(p_source_path);
	if (plan_ids == nullptr || plan_ids->size() != 1) {
		return -1;
	}

	for (const int &plan_id : *plan_ids) {
		return plan_id;
	}

	return -1;
}

Ref<SceneInstantiationPlanNode> SceneInstantiationPlan::_make_node_ref(int p_plan_id) const {
	if (!has_node(p_plan_id)) {
		return Ref<SceneInstantiationPlanNode>();
	}

	Ref<SceneInstantiationPlanNode> node_ref;
	node_ref.instantiate();
	node_ref->_setup(const_cast<SceneInstantiationPlan *>(this), p_plan_id);
	return node_ref;
}

void SceneInstantiationPlan::_detach_from_parent(int p_plan_id) {
	PlanNodeData *node = _get_node_data_w(p_plan_id);
	ERR_FAIL_NULL(node);
	if (node->parent_plan_id < 0) {
		return;
	}

	PlanNodeData *parent = _get_node_data_w(node->parent_plan_id);
	ERR_FAIL_NULL(parent);
	for (int i = 0; i < parent->child_plan_ids.size(); i++) {
		if (parent->child_plan_ids[i] == p_plan_id) {
			parent->child_plan_ids.remove_at(i);
			break;
		}
	}
	node->parent_plan_id = -1;
}

int SceneInstantiationPlan::_duplicate_node_subtree(int p_plan_id, int p_parent_plan_id) {
	const PlanNodeData *source = _get_node_data(p_plan_id);
	ERR_FAIL_NULL_V(source, -1);
	const Vector<int> source_child_plan_ids = source->child_plan_ids;

	PlanNodeData copy = *source;
	copy.plan_id = plan_nodes.size();
	copy.parent_plan_id = p_parent_plan_id;
	copy.child_plan_ids.clear();
	copy.origin = SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_DUPLICATED;
	copy.duplicated_from_plan_id = p_plan_id;
	copy.pruned = false;
	copy.flattened = false;

	const int new_plan_id = copy.plan_id;
	plan_nodes.push_back(copy);
	_index_source_path(new_plan_id, copy.source_path);

	for (int child_plan_id : source_child_plan_ids) {
		const int duplicated_child_plan_id = _duplicate_node_subtree(child_plan_id, new_plan_id);
		if (duplicated_child_plan_id >= 0) {
			plan_nodes.write[new_plan_id].child_plan_ids.push_back(duplicated_child_plan_id);
		}
	}

	return new_plan_id;
}

void SceneInstantiationPlan::_set_node_property(int p_plan_id, const StringName &p_name, const Variant &p_value, bool p_deferred_node_path) {
	PlanNodeData *node = _get_node_data_w(p_plan_id);
	ERR_FAIL_NULL(node);
	node->properties.insert(p_name, p_value);
	if (p_deferred_node_path) {
		node->deferred_node_properties.insert(p_name);
	}
}

void SceneInstantiationPlan::_clear_node_property_override(int p_plan_id, const StringName &p_name) {
	PlanNodeData *node = _get_node_data_w(p_plan_id);
	ERR_FAIL_NULL(node);
	if (node->base_properties.has(p_name)) {
		node->properties.insert(p_name, node->base_properties[p_name]);
	} else {
		node->properties.erase(p_name);
	}
}

void SceneInstantiationPlan::_rename_node(int p_plan_id, const StringName &p_name) {
	PlanNodeData *node = _get_node_data_w(p_plan_id);
	ERR_FAIL_NULL(node);
	node->name = p_name;
}

void SceneInstantiationPlan::_prune_node(int p_plan_id) {
	PlanNodeData *node = _get_node_data_w(p_plan_id);
	ERR_FAIL_NULL(node);
	if (node->parent_plan_id < 0) {
		ERR_FAIL_MSG("Cannot prune the root node of an instantiation plan.");
	}
	_detach_from_parent(p_plan_id);
	_unindex_source_path(p_plan_id, node->source_path);
	node->pruned = true;
}

Array SceneInstantiationPlan::_duplicate_node(int p_plan_id, int p_additional_count) {
	Array duplicated_nodes;
	if (p_additional_count <= 0) {
		return duplicated_nodes;
	}

	PlanNodeData *node = _get_node_data_w(p_plan_id);
	ERR_FAIL_NULL_V(node, duplicated_nodes);
	ERR_FAIL_COND_V_MSG(node->parent_plan_id < 0, duplicated_nodes, "Cannot duplicate the root node of an instantiation plan.");
	const int parent_plan_id = node->parent_plan_id;

	PlanNodeData *parent = _get_node_data_w(parent_plan_id);
	ERR_FAIL_NULL_V(parent, duplicated_nodes);

	int insert_position = parent->child_plan_ids.size();
	for (int i = 0; i < parent->child_plan_ids.size(); i++) {
		if (parent->child_plan_ids[i] == p_plan_id) {
			insert_position = i + 1;
			break;
		}
	}

	for (int i = 0; i < p_additional_count; i++) {
		const int duplicated_plan_id = _duplicate_node_subtree(p_plan_id, parent_plan_id);
		if (duplicated_plan_id < 0) {
			continue;
		}
		parent = _get_node_data_w(parent_plan_id);
		ERR_FAIL_NULL_V(parent, duplicated_nodes);
		parent->child_plan_ids.insert(insert_position + i, duplicated_plan_id);
		duplicated_nodes.push_back(_make_node_ref(duplicated_plan_id));
	}

	return duplicated_nodes;
}

void SceneInstantiationPlan::_flatten_node(int p_plan_id) {
	PlanNodeData *node = _get_node_data_w(p_plan_id);
	ERR_FAIL_NULL(node);
	ERR_FAIL_COND_MSG(node->parent_plan_id < 0, "Cannot flatten the root node of an instantiation plan.");

	PlanNodeData *parent = _get_node_data_w(node->parent_plan_id);
	ERR_FAIL_NULL(parent);

	int insert_position = parent->child_plan_ids.size();
	for (int i = 0; i < parent->child_plan_ids.size(); i++) {
		if (parent->child_plan_ids[i] == p_plan_id) {
			insert_position = i;
			parent->child_plan_ids.remove_at(i);
			break;
		}
	}

	for (int i = 0; i < node->child_plan_ids.size(); i++) {
		const int child_plan_id = node->child_plan_ids[i];
		PlanNodeData *child = _get_node_data_w(child_plan_id);
		ERR_CONTINUE(child == nullptr);
		child->parent_plan_id = parent->plan_id;
		parent->child_plan_ids.insert(insert_position + i, child_plan_id);
	}

	node->child_plan_ids.clear();
	node->parent_plan_id = -1;
	_unindex_source_path(p_plan_id, node->source_path);
	node->flattened = true;
	node->pruned = true;
}

int SceneInstantiationPlan::add_node(const Ref<SceneState> &p_source_state, int p_source_node_idx, int p_parent_plan_id, const NodePath &p_source_path, const String &p_source_scene_path, const StringName &p_name, const StringName &p_type, SceneInstantiationPlanNodeOrigin p_origin, bool p_instance_root) {
	PlanNodeData node;
	node.plan_id = plan_nodes.size();
	node.parent_plan_id = p_parent_plan_id;
	node.source_state = p_source_state;
	node.source_node_idx = p_source_node_idx;
	node.source_path = p_source_path;
	node.owner_path = NodePath();
	node.source_scene_path = p_source_scene_path;
	node.name = p_name;
	node.type = p_type;
	node.origin = p_origin;
	node.instance_root = p_instance_root;

	const int plan_id = node.plan_id;
	plan_nodes.push_back(node);
	_index_source_path(plan_id, node.source_path);

	if (p_parent_plan_id >= 0) {
		PlanNodeData *parent = _get_node_data_w(p_parent_plan_id);
		ERR_FAIL_NULL_V(parent, plan_id);
		parent->child_plan_ids.push_back(plan_id);
	} else if (root_plan_id == -1) {
		root_plan_id = plan_id;
	}

	return plan_id;
}

void SceneInstantiationPlan::set_root_plan_id(int p_plan_id) {
	ERR_FAIL_INDEX(p_plan_id, plan_nodes.size());
	root_plan_id = p_plan_id;
}

void SceneInstantiationPlan::set_node_base_property(int p_plan_id, const StringName &p_name, const Variant &p_value, bool p_deferred_node_path) {
	PlanNodeData *node = _get_node_data_w(p_plan_id);
	ERR_FAIL_NULL(node);
	node->base_properties.insert(p_name, p_value);
	node->properties.insert(p_name, p_value);
	if (p_deferred_node_path) {
		node->deferred_node_properties.insert(p_name);
	}
}

bool SceneInstantiationPlan::has_node(int p_plan_id) const {
	return p_plan_id >= 0 && p_plan_id < plan_nodes.size();
}

int SceneInstantiationPlan::get_root_plan_id() const {
	return root_plan_id;
}

Ref<SceneInstantiationPlanNode> SceneInstantiationPlan::get_root_node() const {
	return _make_node_ref(root_plan_id);
}

Ref<PackedScene> SceneInstantiationPlan::_extract_scene(int p_plan_id) const {
	const PlanNodeData *node = _get_node_data(p_plan_id);
	ERR_FAIL_NULL_V(node, Ref<PackedScene>());
	ERR_FAIL_COND_V(node->pruned || node->flattened, Ref<PackedScene>());
	ERR_FAIL_COND_V(node->source_state.is_null(), Ref<PackedScene>());
	return node->source_state->_extract_runtime_plan_scene(Ref<SceneInstantiationPlan>(const_cast<SceneInstantiationPlan *>(this)), p_plan_id);
}

SceneInstantiationPlanNode::SceneInstantiationPlanNode() {
}

void SceneInstantiationPlanNode::_setup(const Ref<SceneInstantiationPlan> &p_plan, int p_plan_id) {
	plan = p_plan;
	plan_id = p_plan_id;
}

int SceneInstantiationPlanNode::get_plan_id() const {
	return plan_id;
}

StringName SceneInstantiationPlanNode::get_name() const {
	ERR_FAIL_COND_V(plan.is_null(), StringName());
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, StringName());
	return node->name;
}

void SceneInstantiationPlanNode::set_name(const StringName &p_name) {
	ERR_FAIL_COND(plan.is_null());
	plan->_rename_node(plan_id, p_name);
}

StringName SceneInstantiationPlanNode::get_type() const {
	ERR_FAIL_COND_V(plan.is_null(), StringName());
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, StringName());
	return node->type;
}

NodePath SceneInstantiationPlanNode::get_source_path() const {
	ERR_FAIL_COND_V(plan.is_null(), NodePath());
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, NodePath());
	return node->source_path;
}

String SceneInstantiationPlanNode::get_source_scene_path() const {
	ERR_FAIL_COND_V(plan.is_null(), String());
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, String());
	return node->source_scene_path;
}

int SceneInstantiationPlanNode::get_origin() const {
	ERR_FAIL_COND_V(plan.is_null(), ORIGIN_LOCAL);
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, ORIGIN_LOCAL);
	return node->origin;
}

bool SceneInstantiationPlanNode::is_instance_root() const {
	ERR_FAIL_COND_V(plan.is_null(), false);
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, false);
	return node->instance_root;
}

Ref<SceneInstantiationPlanNode> SceneInstantiationPlanNode::get_parent() const {
	ERR_FAIL_COND_V(plan.is_null(), Ref<SceneInstantiationPlanNode>());
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, Ref<SceneInstantiationPlanNode>());
	return plan->_make_node_ref(node->parent_plan_id);
}

int SceneInstantiationPlanNode::get_child_count() const {
	ERR_FAIL_COND_V(plan.is_null(), 0);
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, 0);
	return node->child_plan_ids.size();
}

Ref<SceneInstantiationPlanNode> SceneInstantiationPlanNode::get_child(int p_index) const {
	ERR_FAIL_COND_V(plan.is_null(), Ref<SceneInstantiationPlanNode>());
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, Ref<SceneInstantiationPlanNode>());
	ERR_FAIL_INDEX_V(p_index, node->child_plan_ids.size(), Ref<SceneInstantiationPlanNode>());
	return plan->_make_node_ref(node->child_plan_ids[p_index]);
}

Array SceneInstantiationPlanNode::get_children() const {
	Array children;
	ERR_FAIL_COND_V(plan.is_null(), children);
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, children);
	for (int child_plan_id : node->child_plan_ids) {
		children.push_back(plan->_make_node_ref(child_plan_id));
	}
	return children;
}

bool SceneInstantiationPlanNode::has_property(const StringName &p_name) const {
	ERR_FAIL_COND_V(plan.is_null(), false);
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, false);
	return node->properties.has(p_name);
}

Variant SceneInstantiationPlanNode::get_property(const StringName &p_name, const Variant &p_default) const {
	ERR_FAIL_COND_V(plan.is_null(), p_default);
	const SceneInstantiationPlan::PlanNodeData *node = plan->_get_node_data(plan_id);
	ERR_FAIL_NULL_V(node, p_default);
	if (!node->properties.has(p_name)) {
		return p_default;
	}
	return node->properties[p_name];
}

void SceneInstantiationPlanNode::set_property(const StringName &p_name, const Variant &p_value) {
	ERR_FAIL_COND(plan.is_null());
	plan->_set_node_property(plan_id, p_name, p_value);
}

void SceneInstantiationPlanNode::clear_property_override(const StringName &p_name) {
	ERR_FAIL_COND(plan.is_null());
	plan->_clear_node_property_override(plan_id, p_name);
}

void SceneInstantiationPlanNode::prune() {
	ERR_FAIL_COND(plan.is_null());
	plan->_prune_node(plan_id);
}

Array SceneInstantiationPlanNode::duplicate(int p_additional_count) {
	ERR_FAIL_COND_V(plan.is_null(), Array());
	return plan->_duplicate_node(plan_id, p_additional_count);
}

Ref<PackedScene> SceneInstantiationPlanNode::extract_scene() const {
	ERR_FAIL_COND_V(plan.is_null(), Ref<PackedScene>());
	return plan->_extract_scene(plan_id);
}

void SceneInstantiationPlanNode::flatten_into_parent() {
	ERR_FAIL_COND(plan.is_null());
	plan->_flatten_node(plan_id);
}

Node *SceneState::create_duplicate_node(Node *original_node, const NodeData &n, const StringName *snames, const Variant *props, GenEditState p_edit_state) const {
	Node *duplicate_node = nullptr;
	
	// Create duplicate based on the original node's type and properties
	if (n.instance >= 0) {
		// For instantiated scenes, create a new instance
		if (n.instance & FLAG_INSTANCE_IS_PLACEHOLDER) {
			const String scene_path = props[n.instance & FLAG_MASK];
			if (disable_placeholders) {
				Ref<PackedScene> sdata = ResourceLoader::load(scene_path, "PackedScene");
				if (sdata.is_valid()) {
					duplicate_node = sdata->instantiate(p_edit_state == GEN_EDIT_STATE_DISABLED ? PackedScene::GEN_EDIT_STATE_DISABLED : PackedScene::GEN_EDIT_STATE_INSTANCE);
				}
			} else {
				InstancePlaceholder *ip = memnew(InstancePlaceholder);
				ip->set_instance_path(scene_path);
				duplicate_node = ip;
			}
			if (duplicate_node) {
				duplicate_node->set_scene_instance_load_placeholder(true);
			}
		} else {
			Ref<Resource> res = props[n.instance & FLAG_MASK];
			Ref<PackedScene> sdata = res;
			if (sdata.is_valid()) {
				duplicate_node = sdata->instantiate(p_edit_state == GEN_EDIT_STATE_DISABLED ? PackedScene::GEN_EDIT_STATE_DISABLED : PackedScene::GEN_EDIT_STATE_INSTANCE);
			}
		}
	} else if (n.type != TYPE_INSTANTIATED) {
		// For regular nodes, create new instance of the same class
		Object *obj = ClassDB::instantiate(snames[n.type]);
		duplicate_node = Object::cast_to<Node>(obj);
		if (!duplicate_node && obj) {
			memdelete(obj);
		}
	}
	
	if (duplicate_node) {
		// Copy properties from original node
		int nprop_count = n.properties.size();
		int sname_count = names.size();
		int prop_count = variants.size();
		if (nprop_count) {
			const NodeData::Property *nprops = &n.properties[0];
			for (int j = 0; j < nprop_count; j++) {
				if (nprops[j].name >= 0 && nprops[j].name < sname_count && nprops[j].value >= 0 && nprops[j].value < prop_count) {
					bool valid;
					duplicate_node->set(snames[nprops[j].name], props[nprops[j].value], &valid);
				}
			}
		}
		
		// Add to groups
		for (int j = 0; j < n.groups.size(); j++) {
			if (n.groups[j] >= 0 && n.groups[j] < sname_count) {
				duplicate_node->add_to_group(snames[n.groups[j]], true);
			}
		}
	}
	
	return duplicate_node;
}

String SceneState::build_node_path(int node_idx, const Vector<NodeData> &nodes_, const StringName *snames, const Vector<NodePath> &node_paths_) const {
	if (node_idx == 0) {
		return "."; // Root node
	}
	
	// For NodePath-based parents, we can directly construct the path
	int parent_raw = nodes_[node_idx].parent;
	if (parent_raw != -1 && (parent_raw & FLAG_ID_IS_PATH)) {
		NodePath parent_path = node_paths_[parent_raw & FLAG_MASK];
		String parent_path_str = String(parent_path);
		if (parent_path_str == ".") {
			return "./" + String(snames[nodes_[node_idx].name]);
		} else {
			return parent_path_str + "/" + String(snames[nodes_[node_idx].name]);
		}
	}
	
	// Fallback for index-based parents (build path recursively)
	Vector<String> path_parts;
	int current_idx = node_idx;
	
	while (current_idx != -1 && current_idx != 0) {
		path_parts.push_back(String(snames[nodes_[current_idx].name]));
		
		int parent_raw_local = nodes_[current_idx].parent;
		if (parent_raw_local == -1) {
			break;
		}
		if (parent_raw_local & FLAG_ID_IS_PATH) {
			// We've hit a NodePath parent, stop here
			break;
		} else {
			current_idx = parent_raw_local & FLAG_MASK;
			if (current_idx >= nodes_.size()) break;
		}
	}
	
	// Build the path string
	String result = ".";
	for (int i = path_parts.size() - 1; i >= 0; i--) {
		result += "/" + path_parts[i];
	}
	
	return result;
}

Node *SceneState::_instantiate_legacy(GenEditState p_edit_state) const {
	// Nodes where instantiation failed (because something is missing.)
	List<Node *> stray_instances;

#define NODE_FROM_ID(p_name, p_id) \
	Node *p_name; \
	if (p_id & FLAG_ID_IS_PATH) { \
		NodePath np = node_paths[p_id & FLAG_MASK]; \
		p_name = ret_nodes[0]->get_node_or_null(np); \
		if (!p_name) { \
			p_name = _recover_node_path_index(ret_nodes[0], p_id & FLAG_MASK); \
		} \
	} else { \
		ERR_FAIL_INDEX_V(p_id & FLAG_MASK, nc, nullptr); \
		p_name = ret_nodes[p_id & FLAG_MASK]; \
	}

	int nc = nodes.size();
	ERR_FAIL_COND_V_MSG(nc == 0, nullptr, vformat("Failed to instantiate scene state of \"%s\", node count is 0. Make sure the PackedScene resource is valid.", path));

	const StringName *snames = nullptr;
	int sname_count = names.size();
	if (sname_count) {
		snames = &names[0];
	}

	const Variant *props = nullptr;
	int prop_count = variants.size();
	if (prop_count) {
		props = &variants[0];
	}

	//Vector<Variant> properties;

	const NodeData *nd = &nodes[0];

	Node **ret_nodes = (Node **)alloca(sizeof(Node *) * nc);
	for (int i = 0; i < nc; i++) {
		ret_nodes[i] = nullptr;
	}

	bool gen_node_path_cache = p_edit_state != GEN_EDIT_STATE_DISABLED && node_path_cache.is_empty();

	HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> resources_local_to_scenes; // Record the mappings in sub-scenes.

	LocalVector<DeferredNodePathProperties> deferred_node_paths;

	bool deep_search_warned = false;

	Vector<bool> skip_node_init;
	Vector<bool> &skip_node = skip_node_init;
	skip_node.resize(nc);
	for (int k = 0; k < nc; ++k) skip_node.write[k] = false;

	// Map to store property overrides for nodes: node_index -> Array of property override dictionaries (for duplicates)
	HashMap<int, Array> property_overrides_per_node;
	// Map to store how many instances each node should have: node_index -> count
	HashMap<int, int> node_duplication_count;
	// Map to track which nodes are descendants of duplicated nodes: descendant_node_index -> parent_duplicate_node_index
	HashMap<int, int> descendant_of_duplicate;
	// Map to store additional duplicate nodes that have been created: original_node_index -> Array of duplicate nodes
	HashMap<int, Array> additional_duplicate_nodes;

	for (int i = 0; i < nc; i++) {
		const NodeData &n = nd[i];

		Node *parent = nullptr;
		String old_parent_path;

		if (i > 0) {

			if (skip_node.size() > i && skip_node[i]) {
				ret_nodes[i] = nullptr;
				continue;
			}

			ERR_FAIL_COND_V_MSG(n.parent == -1, nullptr, vformat("Invalid scene: node %s does not specify its parent node.", snames[n.name]));
			NODE_FROM_ID(nparent, n.parent);
#ifdef DEBUG_ENABLED
			if (!nparent && (n.parent & FLAG_ID_IS_PATH)) {
				WARN_PRINT(String("Parent path '" + String(node_paths[n.parent & FLAG_MASK]) + "' for node '" + String(snames[n.name]) + "' has vanished when instantiating: '" + get_path() + "'.").ascii().get_data());
				old_parent_path = String(node_paths[n.parent & FLAG_MASK]).trim_prefix("./").replace_char('/', '@');
				nparent = ret_nodes[0];
			}
#endif
			parent = nparent;
		} else {
			// i == 0 is root node.
			ERR_FAIL_COND_V_MSG(n.parent != -1, nullptr, vformat("Invalid scene: root node %s cannot specify a parent node.", snames[n.name]));
			ERR_FAIL_COND_V_MSG(n.type == TYPE_INSTANTIATED && base_scene_idx < 0, nullptr, vformat("Invalid scene: root node %s in an instance, but there's no base scene.", snames[n.name]));
		}

		Node *node = nullptr;
		MissingNode *missing_node = nullptr;
		bool is_inherited_scene = false;

		if (i == 0 && base_scene_idx >= 0) {
			// Scene inheritance on root node.
			Ref<PackedScene> sdata = props[base_scene_idx];
			ERR_FAIL_COND_V(sdata.is_null(), nullptr);
			node = sdata->instantiate(p_edit_state == GEN_EDIT_STATE_DISABLED ? PackedScene::GEN_EDIT_STATE_DISABLED : PackedScene::GEN_EDIT_STATE_INSTANCE); //only main gets main edit state
			ERR_FAIL_NULL_V(node, nullptr);
			if (p_edit_state != GEN_EDIT_STATE_DISABLED) {
				node->set_scene_inherited_state(sdata->get_state());
			}
			is_inherited_scene = true;
		} else if (n.instance >= 0) {
			// Instance a scene into this node.
			if (n.instance & FLAG_INSTANCE_IS_PLACEHOLDER) {
				const String scene_path = props[n.instance & FLAG_MASK];
				if (disable_placeholders) {
					Ref<PackedScene> sdata = ResourceLoader::load(scene_path, "PackedScene");
					if (sdata.is_valid()) {
						node = sdata->instantiate(p_edit_state == GEN_EDIT_STATE_DISABLED ? PackedScene::GEN_EDIT_STATE_DISABLED : PackedScene::GEN_EDIT_STATE_INSTANCE);
						ERR_FAIL_NULL_V(node, nullptr);
					} else if (ResourceLoader::is_creating_missing_resources_if_class_unavailable_enabled()) {
						missing_node = memnew(MissingNode);
						missing_node->set_original_scene(scene_path);
						missing_node->set_recording_properties(true);
						node = missing_node;
					} else {
						ERR_FAIL_V_MSG(nullptr, "Placeholder scene is missing.");
					}
				} else {
					InstancePlaceholder *ip = memnew(InstancePlaceholder);
					ip->set_instance_path(scene_path);
					node = ip;
				}
				node->set_scene_instance_load_placeholder(true);
			} else {
				Ref<Resource> res = props[n.instance & FLAG_MASK];
				Ref<PackedScene> sdata = res;
				if (sdata.is_valid()) {
					node = sdata->instantiate(p_edit_state == GEN_EDIT_STATE_DISABLED ? PackedScene::GEN_EDIT_STATE_DISABLED : PackedScene::GEN_EDIT_STATE_INSTANCE);
					ERR_FAIL_NULL_V_MSG(node, nullptr, vformat("Failed to load scene dependency: \"%s\". Make sure the required scene is valid.", sdata->get_path()));
				} else if (ResourceLoader::is_creating_missing_resources_if_class_unavailable_enabled()) {
					missing_node = memnew(MissingNode);
#ifdef TOOLS_ENABLED
					if (res.is_valid()) {
						missing_node->set_original_scene(res->get_meta("__load_path__", ""));
					}
#endif
					missing_node->set_recording_properties(true);
					node = missing_node;
				} else {
					ERR_FAIL_V_MSG(nullptr, "Scene instance is missing.");
				}
			}

		} else if (n.type == TYPE_INSTANTIATED) {
			// Get the node from somewhere, it likely already exists from another instance.
			if (parent) {
				node = parent->_get_child_by_name(snames[n.name]);
				if (i < ids.size()) {
					if (!node) {
						// Can't get by name, try to fetch by ID. This is slow, but should be fixed after re-save.
						int32_t id = ids[i];
						if (id != Node::UNIQUE_SCENE_ID_UNASSIGNED) {
							if (!deep_search_warned) {
								WARN_PRINT(vformat("%sA node in the scene this one inherits from has been removed or moved, so a recovery process needs to take place. Please re-save this scene to avoid the cost of this process next time.", !get_path().is_empty() ? get_path() + ": " : ""));
								deep_search_warned = true;
							}
							Node *base = parent;
							while (base != ret_nodes[0] && !base->is_instance()) {
								base = base->get_parent();
							}
							node = _find_node_by_id(base, base, id);
						}
					} else {
						if (ids[i] != node->get_unique_scene_id()) {
							// This may be a scene that did not originally have ids and
							// was saved before the parent, so force the id to match the
							// parent scene node id.
							ids.write[i] = node->get_unique_scene_id();
						}
					}
				}
#ifdef DEBUG_ENABLED
				if (!node) {
					WARN_PRINT(String("Node '" + String(ret_nodes[0]->get_path_to(parent)) + "/" + String(snames[n.name]) + "' was modified from inside an instance, but it has vanished.").ascii().get_data());
				}
#endif
			}
		} else {
			// Node belongs to this scene and must be created.
			Object *obj = ClassDB::instantiate(snames[n.type]);

			node = Object::cast_to<Node>(obj);

			if (!node) {
				if (obj) {
					memdelete(obj);
					obj = nullptr;
				}

				if (ResourceLoader::is_creating_missing_resources_if_class_unavailable_enabled()) {
					missing_node = memnew(MissingNode);
					missing_node->set_original_class(snames[n.type]);
					missing_node->set_recording_properties(true);
					node = missing_node;
					obj = missing_node;
				} else {
					WARN_PRINT(vformat("Node %s of type %s cannot be created. A placeholder will be created instead.", snames[n.name], snames[n.type]).ascii().get_data());
					if (n.parent >= 0 && n.parent < nc && ret_nodes[n.parent]) {
						if (Object::cast_to<Control>(ret_nodes[n.parent])) {
							obj = memnew(Control);
						} else if (Object::cast_to<Node2D>(ret_nodes[n.parent])) {
							obj = memnew(Node2D);
#ifndef _3D_DISABLED
						} else if (Object::cast_to<Node3D>(ret_nodes[n.parent])) {
							obj = memnew(Node3D);
#endif // _3D_DISABLED
						}
					}

					if (!obj) {
						obj = memnew(Node);
					}

					node = Object::cast_to<Node>(obj);
				}
			}
		}

		if (node) {
			if (i < ids.size()) {
				node->set_unique_scene_id(ids[i]);
			}
			// may not have found the node (part of instantiated scene and removed)
			// if found all is good, otherwise ignore

			//properties
			int nprop_count = n.properties.size();
			if (nprop_count) {
				const NodeData::Property *nprops = &n.properties[0];

				Dictionary missing_resource_properties;

				for (int j = 0; j < nprop_count; j++) {
					bool valid;

					ERR_FAIL_INDEX_V(nprops[j].value, prop_count, nullptr);

					if (nprops[j].name & FLAG_PATH_PROPERTY_IS_NODE) {
						if (!Engine::get_singleton()->is_editor_hint() && node->get_scene_instance_load_placeholder()) {
							// We cannot know if the referenced nodes exist yet, so instead of deferring, we write the NodePaths directly.

							uint32_t name_idx = nprops[j].name & (FLAG_PATH_PROPERTY_IS_NODE - 1);
							ERR_FAIL_UNSIGNED_INDEX_V(name_idx, (uint32_t)sname_count, nullptr);

							node->set(snames[name_idx], props[nprops[j].value], &valid);
							continue;
						}

						uint32_t name_idx = nprops[j].name & (FLAG_PATH_PROPERTY_IS_NODE - 1);
						ERR_FAIL_UNSIGNED_INDEX_V(name_idx, (uint32_t)sname_count, nullptr);

						DeferredNodePathProperties dnp;
						dnp.value = props[nprops[j].value];
						dnp.base = node->get_instance_id();
						dnp.property = snames[name_idx];
						deferred_node_paths.push_back(dnp);
						continue;
					}

					ERR_FAIL_INDEX_V(nprops[j].name, sname_count, nullptr);

					if (snames[nprops[j].name] == CoreStringName(script)) {
						//work around to avoid old script variables from disappearing, should be the proper fix to:
						//https://github.com/godotengine/godot/issues/2958

						//store old state
						List<Pair<StringName, Variant>> old_state;
						if (node->get_script_instance()) {
							node->get_script_instance()->get_property_state(old_state);
						}

#ifdef TOOLS_ENABLED
						const Ref<Script> value_as_script = props[nprops[j].value];
						// It is possible that the user changed an existing script to abstract after it was attached to a node.
						// When this happens, the user needs to fix it. See https://github.com/godotengine/godot/issues/109171
						if (value_as_script.is_valid() && value_as_script->is_abstract()) {
							const String global_class_name = value_as_script->get_global_name();
							if (global_class_name.is_empty()) {
								ERR_PRINT("Node \"" + snames[n.name] + "\" previously had a script, but that script is now abstract. Please assign a different script (right-click -> Attach Script...) or change the node to a different type (right-click -> Change Type...) to fix this, then re-save the scene.");
							} else {
								ERR_PRINT("Node \"" + snames[n.name] + "\" previously had a class of type \"" + global_class_name + "\", but that class is now abstract. Please assign a different script (right-click -> Attach Script...) or change the node to a different type (right-click -> Change Type...) to fix this, then re-save the scene.");
							}
							callable_mp((Object *)node, &Object::remove_meta).call_deferred(SceneStringName(_custom_type_script));
						} else {
							node->set_script(props[nprops[j].value]);
						}
#else
						node->set_script(props[nprops[j].value]);
#endif // TOOLS_ENABLED

						//restore old state for new script, if exists
						for (const Pair<StringName, Variant> &E : old_state) {
							node->set(E.first, E.second);
						}
					} else {
						Variant value = props[nprops[j].value];

						if (value.get_type() == Variant::OBJECT) {
							//handle resources that are local to scene by duplicating them if needed
							Ref<Resource> res = value;
							if (res.is_valid()) {
								value = make_local_resource(value, n, resources_local_to_scenes, node, snames[nprops[j].name], i, ret_nodes, p_edit_state);
							}
						} else {
							// Making sure that instances of inherited scenes don't share the same
							// reference between them.
							if (is_inherited_scene) {
								value = value.duplicate(true);
							}
						}

						if (value.get_type() == Variant::ARRAY) {
							Array set_array = value;
							bool is_get_valid = false;
							Variant get_value = node->get(snames[nprops[j].name], &is_get_valid);

							if (is_get_valid && get_value.get_type() == Variant::ARRAY) {
								Array get_array = get_value;
								if (set_array.is_same_typed(get_array)) {
									set_array = set_array.duplicate();
								} else {
									set_array = Array(set_array, get_array.get_typed_builtin(), get_array.get_typed_class_name(), get_array.get_typed_script());
								}
							}

							value = setup_resources_in_array(set_array, n, resources_local_to_scenes, node, snames[nprops[j].name], i, ret_nodes, p_edit_state);
						}

						if (value.get_type() == Variant::DICTIONARY) {
							Dictionary set_dict = value;
							bool is_get_valid = false;
							Variant get_value = node->get(snames[nprops[j].name], &is_get_valid);

							if (is_get_valid && get_value.get_type() == Variant::DICTIONARY) {
								Dictionary get_dict = get_value;
								if (set_dict.is_same_typed(get_dict)) {
									set_dict = set_dict.duplicate();
								} else {
									set_dict = Dictionary(set_dict, get_dict.get_typed_key_builtin(), get_dict.get_typed_key_class_name(), get_dict.get_typed_key_script(), get_dict.get_typed_value_builtin(), get_dict.get_typed_value_class_name(), get_dict.get_typed_value_script());
								}
							}

							value = setup_resources_in_dictionary(set_dict, n, resources_local_to_scenes, node, snames[nprops[j].name], i, ret_nodes, p_edit_state);
						}

						bool set_valid = true;
						if (ResourceLoader::is_creating_missing_resources_if_class_unavailable_enabled() && value.get_type() == Variant::OBJECT) {
							Ref<MissingResource> mr = value;
							if (mr.is_valid()) {
								missing_resource_properties[snames[nprops[j].name]] = mr;
								set_valid = false;
							}
						}

						if (set_valid) {
							node->set(snames[nprops[j].name], value, &valid);
						}
						if (p_edit_state == GEN_EDIT_STATE_INSTANCE && value.get_type() != Variant::OBJECT) {
							value = value.duplicate(true); // Duplicate arrays and dictionaries for the editor.
						}
					}
				}
				
				// Apply property overrides from _filter_scene_children if available
				if (!Engine::get_singleton()->is_editor_hint() && property_overrides_per_node.has(i)) {
					Array override_list = property_overrides_per_node[i];
					// Apply the first entry's overrides to the original node (index 0)
					if (override_list.size() > 0) {
						Dictionary override_props = override_list[0];
						for (const KeyValue<Variant, Variant> &kv : override_props) {
							bool valid;
							node->set(kv.key, kv.value, &valid);
							if (!valid) {
								WARN_PRINT(vformat("Failed to set override property '%s' on node '%s'.", kv.key, node->get_name()));
							}
						}
					}
				}
				if (!missing_resource_properties.is_empty()) {
					node->set_meta(META_MISSING_RESOURCES, missing_resource_properties);
				}
			}

			//name

			//groups
			for (int j = 0; j < n.groups.size(); j++) {
				ERR_FAIL_INDEX_V(n.groups[j], sname_count, nullptr);
				node->add_to_group(snames[n.groups[j]], true);
			}

			if (n.instance >= 0 || n.type != TYPE_INSTANTIATED || i == 0) {
				//if node was not part of instance, must set its name, parenthood and ownership
				if (i > 0) {
					if (parent) {
						bool pending_add = true;
#ifdef TOOLS_ENABLED
						if (Engine::get_singleton()->is_editor_hint()) {
							Node *existing = parent->_get_child_by_name(snames[n.name]);
							if (existing) {
								// There's already a node in the same parent with the same name.
								// This means that somehow the node was added both to the scene being
								// loaded and another one instantiated in the former, maybe because of
								// manual editing, or a bug in scene saving, or a loophole in the workflow
								// (with any of the bugs possibly already fixed).
								// Bring consistency back by letting it be assigned a non-clashing name.
								// This simple workaround at least avoids leaks and helps the user realize
								// something awkward has happened.
								if (instantiation_warn_notify) {
									instantiation_warn_notify(vformat(
											TTR("An incoming node's name clashes with %s already in the scene (presumably, from a more nested instance).\nThe less nested node will be renamed. Please fix and re-save the scene."),
											ret_nodes[0]->get_path_to(existing)));
								}
								node->set_name(snames[n.name]);
								parent->add_child(node, true);
								pending_add = false;
							}
						}
#endif
						if (pending_add) {
							parent->_add_child_nocheck(node, snames[n.name]);
						}
						if (n.index >= 0 && n.index < parent->get_child_count() - 1) {
							parent->move_child(node, n.index);
						}
					} else {
						//it may be possible that an instantiated scene has changed
						//and the node has nowhere to go anymore
						stray_instances.push_back(node); //can't be added, go to stray list
					}
				} else {
					if (Engine::get_singleton()->is_editor_hint()) {
						//validate name if using editor, to avoid broken
						node->set_name(snames[n.name]);
					} else {
						node->_set_name_nocheck(snames[n.name]);
					}
				}
			}

			if (!old_parent_path.is_empty()) {
				node->set_name(old_parent_path + "#" + node->get_name());
			}

			if (n.owner >= 0) {
				NODE_FROM_ID(owner, n.owner);
				if (owner) {
					node->_set_owner_nocheck(owner);
					if (node->data.unique_name_in_owner) {
						node->_acquire_unique_name_in_owner();
					}
				}
			}

			// We only want to deal with pinned flag if instantiating as pure main (no instance, no inheriting.)
			if (p_edit_state == GEN_EDIT_STATE_MAIN) {
				_sanitize_node_pinned_properties(node);
			} else {
				node->remove_meta("_edit_pinned_properties_");
			}

			// Set ret_nodes[i] before filter logic so child-finding can resolve parents via NODE_FROM_ID
			ret_nodes[i] = node;

			// BEGIN procedural child filtering logic
			// Only run if node is valid, and not in editor

			if (!Engine::get_singleton()->is_editor_hint() && node && node->has_method("_filter_scene_children")) {
				Array child_infos;
				Vector<int> child_node_indices; // Track which node indices are children
				const String current_node_path = build_node_path(i, nodes, snames, node_paths);
				for (int child_idx = 0; child_idx < nc; ++child_idx) {
					const NodeData &child_n = nodes[child_idx];
					if (child_n.parent == -1) {
						continue; // Skip nodes without a parent
					}

					bool is_direct_child = false;
					if (child_n.parent & FLAG_ID_IS_PATH) {
						is_direct_child = String(node_paths[child_n.parent & FLAG_MASK]) == current_node_path;
					} else {
						is_direct_child = (child_n.parent & FLAG_MASK) == i;
					}

					if (is_direct_child) {
						Dictionary info;
						info["id"] = child_idx; // Use node index as unique id
						info["name"] = snames[child_n.name];
						// Handle type properly - check for TYPE_INSTANTIATED flag
						if (child_n.type == TYPE_INSTANTIATED) {
							info["type"] = "TYPE_INSTANTIATED";
						} else {
							info["type"] = snames[child_n.type];
						}
						// Add properties dictionary
						Dictionary prop_dict;
						for (int p = 0; p < child_n.properties.size(); ++p) {
							int prop_name_idx = child_n.properties[p].name;
							int prop_value_idx = child_n.properties[p].value;
							if (prop_name_idx >= 0 && prop_name_idx < sname_count && prop_value_idx >= 0 && prop_value_idx < variants.size()) {
								StringName prop_name = snames[prop_name_idx];
								Variant prop_value = variants[prop_value_idx];
								prop_dict[prop_name] = prop_value;
							}
						}
						info["properties"] = prop_dict;
						child_infos.push_back(info);
						child_node_indices.push_back(child_idx);
					}
				}

				Variant filter_result = node->call("_filter_scene_children", child_infos);

				Array filtered_infos;
				if (filter_result.get_type() == Variant::ARRAY) {
					filtered_infos = filter_result;
				} else {
					filtered_infos = child_infos;
				}
				
				// Create mapping from original node IDs to filtered entries (supporting duplicates)
				HashMap<int, Vector<Dictionary>> id_to_filtered_entries;
				Vector<int> allowed_node_ids;
				
				for (int j = 0; j < filtered_infos.size(); ++j) {
					Dictionary info = filtered_infos[j];
					if (info.has("id")) {
						int node_id = info["id"];
						if (!id_to_filtered_entries.has(node_id)) {
							id_to_filtered_entries[node_id] = Vector<Dictionary>();
							allowed_node_ids.push_back(node_id);
						}
						id_to_filtered_entries[node_id].push_back(info);
					}
				}
				// NodePath-based exclusion: collect excluded NodePaths for nodes not in allowed_node_ids
				Vector<String> excluded_paths;
				for (int idx = 0; idx < child_node_indices.size(); ++idx) {
					int child_idx = child_node_indices[idx];
					
					bool allowed = false;
					for (int a = 0; a < allowed_node_ids.size(); ++a) {
						if (allowed_node_ids[a] == child_idx) {
							allowed = true;
							break;
						}
					}
					
					if (!allowed) {
						excluded_paths.push_back(build_node_path(child_idx, nodes, snames, node_paths));
					}
				}
				// Mark all nodes whose path starts with any excluded NodePath
				for (int idx = 0; idx < nc; ++idx) {
					if (skip_node[idx]) continue;
					const String node_path = build_node_path(idx, nodes, snames, node_paths);
					for (int ep = 0; ep < excluded_paths.size(); ++ep) {
						const String &excluded_path = excluded_paths[ep];
						if (node_path == excluded_path || node_path.begins_with(excluded_path + "/")) {
							skip_node.write[idx] = true;
							break;
						}
					}
				}
				
				// Additional pass: mark all descendants of skipped nodes
				// This handles cases where nodes are skipped but their children weren't caught by the path-based exclusion
				bool changed = true;
				while (changed) {
					changed = false;
					for (int idx = 0; idx < nc; ++idx) {
						if (skip_node[idx]) continue; // Already skipped
						
						// Check if this node's parent is skipped
						int parent_raw = nodes[idx].parent;
						if (parent_raw != -1) {
							if (parent_raw & FLAG_ID_IS_PATH) {
								// For NodePath parents, we need to find the actual parent node index
								// This is more complex, but for now we rely on the path-based exclusion above
								continue;
							} else {
								int parent_idx = parent_raw & FLAG_MASK;
								if (parent_idx < nc && skip_node[parent_idx]) {
									skip_node.write[idx] = true;
									changed = true;
								}
							}
						}
					}
				}
				
				// Store property overrides and duplication info for later application
				for (int j = 0; j < filtered_infos.size(); ++j) {
					Dictionary info = filtered_infos[j];
					if (info.has("id")) {
						int node_id = info["id"];
						
						// Initialize arrays if not present
						if (!property_overrides_per_node.has(node_id)) {
							property_overrides_per_node[node_id] = Array();
							node_duplication_count[node_id] = 0;
						}
						
						// Add property overrides for this instance
						Dictionary override_props;
						if (info.has("properties")) {
							override_props = info["properties"];
						}
						property_overrides_per_node[node_id].push_back(override_props);
						node_duplication_count[node_id]++;
					}
				}
				
				// Mark all descendants of duplicated nodes for duplication
				for (const KeyValue<int, int> &kv : node_duplication_count) {
					int duplicate_parent_id = kv.key;
					if (kv.value > 1) { // Only process nodes that are actually duplicated
						// Find all descendants of this duplicated node
						for (int desc_idx = 0; desc_idx < nc; ++desc_idx) {
							if (desc_idx == duplicate_parent_id) continue;
							
							// Check if desc_idx is a descendant of duplicate_parent_id
							bool is_descendant = false;
							int current_parent_raw = nodes[desc_idx].parent;
							
							// Skip nodes with no parent (like root node)
							if (current_parent_raw == -1) continue;
							
							// For NodePath-based hierarchies, we need a different approach
							if (current_parent_raw & FLAG_ID_IS_PATH) {
								NodePath parent_path = node_paths[current_parent_raw & FLAG_MASK];
								String parent_path_str = String(parent_path);
								
								// Build the expected path for the duplicate parent node
								String duplicate_parent_path = build_node_path(duplicate_parent_id, nodes, snames, node_paths);
								
								// Check if the parent path is exactly the duplicate parent path
								// This means this node is a direct child of the duplicated parent
								if (parent_path_str == duplicate_parent_path) {
									descendant_of_duplicate[desc_idx] = duplicate_parent_id;
									is_descendant = true;
								}
								// Also check if the parent path starts with the duplicate parent path followed by "/"
								// This catches deeper descendants
								else if (parent_path_str.begins_with(duplicate_parent_path + "/")) {
									descendant_of_duplicate[desc_idx] = duplicate_parent_id;
									is_descendant = true;
								}
							} else {
								// Handle direct index parents (original logic)
								while (current_parent_raw != -1) {
									int parent_node_idx = current_parent_raw & FLAG_MASK;
									if (parent_node_idx >= nc) break; // Safety check
									
									if (parent_node_idx == duplicate_parent_id) {
										descendant_of_duplicate[desc_idx] = duplicate_parent_id;
										is_descendant = true;
										break;
									}
									
									current_parent_raw = nodes[parent_node_idx].parent;
								}
							}
						}
					}
				}
			}
			// END procedural child filtering logic
		}

		if (missing_node) {
			missing_node->set_recording_properties(false);
		}

		// Handle descendant duplication - create additional instances for children of duplicated nodes
		if (!Engine::get_singleton()->is_editor_hint() && node && descendant_of_duplicate.has(i)) {
			int duplicate_parent_idx = descendant_of_duplicate[i];
			if (additional_duplicate_nodes.has(duplicate_parent_idx)) {
				Array parent_duplicates = additional_duplicate_nodes[duplicate_parent_idx];
				
				// Create additional instances of this child node for each duplicate parent
				for (int dup_idx = 0; dup_idx < parent_duplicates.size(); dup_idx++) {
					Node *duplicate_parent_node = Object::cast_to<Node>(parent_duplicates[dup_idx]);
					if (duplicate_parent_node) {
						Node *duplicate_child = create_duplicate_node(node, n, snames, props, p_edit_state);
						if (duplicate_child) {
							// Apply property overrides if available
							if (property_overrides_per_node.has(i) && (dup_idx + 1) < property_overrides_per_node[i].size()) {
								Dictionary override_props = property_overrides_per_node[i][dup_idx + 1];
								for (const KeyValue<Variant, Variant> &kv : override_props) {
									bool valid;
									duplicate_child->set(kv.key, kv.value, &valid);
								}
							}
							
							// Set name from user-provided name or use suffix
							String child_name;
							if (property_overrides_per_node.has(i) && (dup_idx + 1) < property_overrides_per_node[i].size()) {
								Dictionary override_props = property_overrides_per_node[i][dup_idx + 1];
								if (override_props.has("name")) {
									child_name = String(override_props["name"]);
								}
							}
							if (child_name.is_empty()) {
								String base_name = String(snames[n.name]);
								child_name = base_name + "_" + String::num(dup_idx + 1);
							}
							duplicate_child->set_name(child_name);
							
							// Add to the duplicate parent
							duplicate_parent_node->add_child(duplicate_child, true);
							
							// Store this duplicate for potential children
							if (!additional_duplicate_nodes.has(i)) {
								additional_duplicate_nodes[i] = Array();
							}
							additional_duplicate_nodes[i].push_back(duplicate_child);
						}
					}
				}
			}
		}

		// Handle node duplication after the original node is fully set up
		if (!Engine::get_singleton()->is_editor_hint() && node && node_duplication_count.has(i) && node_duplication_count[i] > 1) {
			// Only duplicate root-level nodes that were directly specified in filtering
			// (not descendants of other duplicated nodes)
			if (!descendant_of_duplicate.has(i)) {
				Array duplicates;
				// Create additional instances for duplication
				for (int dup_idx = 1; dup_idx < node_duplication_count[i]; dup_idx++) {
					Node *duplicate_node = create_duplicate_node(node, n, snames, props, p_edit_state);
					
					if (duplicate_node) {
						// Apply duplicate-specific property overrides
						if (property_overrides_per_node.has(i)) {
							Array override_list = property_overrides_per_node[i];
							if (dup_idx < override_list.size()) {
								Dictionary override_props = override_list[dup_idx];
								for (const KeyValue<Variant, Variant> &kv : override_props) {
									bool valid;
									duplicate_node->set(kv.key, kv.value, &valid);
								}
							}
						}
						
						// Set name from user-provided name or use suffix
						String node_name;
						if (property_overrides_per_node.has(i)) {
							Array override_list = property_overrides_per_node[i];
							if (dup_idx < override_list.size()) {
								Dictionary override_props = override_list[dup_idx];
								if (override_props.has("name")) {
									node_name = String(override_props["name"]);
								}
							}
						}
						if (node_name.is_empty()) {
							String base_name = String(snames[n.name]);
							node_name = base_name + "_" + String::num(dup_idx);
						}
						duplicate_node->set_name(node_name);
						
						// Add to parent
						if (parent) {
							parent->add_child(duplicate_node, true);
							// Position after the original node
							if (n.index >= 0) {
								parent->move_child(duplicate_node, n.index + dup_idx);
							}
						}
						
						duplicates.push_back(duplicate_node);
					}
				}
				
				// Store the duplicate nodes for later use when creating their children
				if (duplicates.size() > 0) {
					additional_duplicate_nodes[i] = duplicates;
				}
			}
		}

		if (node && gen_node_path_cache && ret_nodes[0]) {
			NodePath n2 = ret_nodes[0]->get_path_to(node);
			node_path_cache[n2] = i;
		}
	}

	for (const DeferredNodePathProperties &dnp : deferred_node_paths) {
		// Replace properties stored as NodePaths with actual Nodes.
		Node *base = ObjectDB::get_instance<Node>(dnp.base);
		ERR_CONTINUE_EDMSG(!base, vformat("Failed to set deferred property '%s' as the base node disappeared.", dnp.property));
		if (dnp.value.get_type() == Variant::ARRAY) {
			Array paths = dnp.value;

			bool valid;
			Array array = base->get(dnp.property, &valid);
			ERR_CONTINUE_EDMSG(!valid, vformat("Failed to get property '%s' from node '%s'.", dnp.property, base->get_name()));
			array = array.duplicate();

			array.resize(paths.size());
			for (int i = 0; i < array.size(); i++) {
				array.set(i, base->get_node_or_null(paths[i]));
			}
			base->set(dnp.property, array);
		} else if (dnp.value.get_type() == Variant::DICTIONARY) {
			Dictionary paths = dnp.value;

			bool valid;
			Dictionary dict = base->get(dnp.property, &valid);
			ERR_CONTINUE_EDMSG(!valid, vformat("Failed to get property '%s' from node '%s'.", dnp.property, base->get_name()));
			dict = dict.duplicate();
			bool convert_key = dict.get_typed_key_builtin() == Variant::OBJECT &&
					ClassDB::is_parent_class(dict.get_typed_key_class_name(), "Node");
			bool convert_value = dict.get_typed_value_builtin() == Variant::OBJECT &&
					ClassDB::is_parent_class(dict.get_typed_value_class_name(), "Node");

			for (const KeyValue<Variant, Variant> &kv : paths) {
				Variant key = kv.key;
				if (convert_key) {
					key = base->get_node_or_null(key);
				}
				Variant value = kv.value;
				if (convert_value) {
					value = base->get_node_or_null(value);
				}
				dict[key] = value;
			}
			base->set(dnp.property, dict);
		} else {
			base->set(dnp.property, base->get_node_or_null(dnp.value));
		}
	}

	for (KeyValue<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &E : resources_local_to_scenes) {
		for (KeyValue<Ref<Resource>, Ref<Resource>> &R : E.value) {
			R.value->setup_local_to_scene(); // Setup may be required for the resource to work properly.
		}
	}

	//do connections

	int cc = connections.size();
	const ConnectionData *cdata = connections.ptr();

	for (int i = 0; i < cc; i++) {
		const ConnectionData &c = cdata[i];
		//ERR_FAIL_INDEX_V( c.from, nc, nullptr );
		//ERR_FAIL_INDEX_V( c.to, nc, nullptr );

		NODE_FROM_ID(cfrom, c.from);
		NODE_FROM_ID(cto, c.to);

		if (!cfrom || !cto) {
			continue;
		}

		Callable callable(cto, snames[c.method]);

		Array binds;

		for (int bind : c.binds) {
			binds.push_back(props[bind]);
		}

		if (!binds.is_empty()) {
			callable = callable.bindv(binds);
		}

		if (c.unbinds > 0) {
			callable = callable.unbind(c.unbinds);
		}

		cfrom->connect(snames[c.signal], callable, CONNECT_PERSIST | c.flags | (p_edit_state == GEN_EDIT_STATE_MAIN ? 0 : CONNECT_INHERITED));
	}

	//Node *s = ret_nodes[0];

	//remove nodes that could not be added, likely as a result that
	while (stray_instances.size()) {
		memdelete(stray_instances.front()->get());
		stray_instances.pop_front();
	}

	for (int i = 0; i < editable_instances.size(); i++) {
		Node *ei = ret_nodes[0]->get_node_or_null(editable_instances[i]);
		if (ei) {
			ret_nodes[0]->set_editable_instance(ei, true);
		}
	}

	// Apply exposed_to_owner flags from the scene state.
	for (int i = 0; i < exposed_children.size(); i++) {
		Node *ec = ret_nodes[0]->get_node_or_null(exposed_children[i]);
		if (ec) {
			ec->set_exposed_to_owner(true);
		}
	}
	ret_nodes[0]->_set_foreign_exposed_node_paths_to_owner(exposed_children);

	return ret_nodes[0];
}

Node *SceneState::_instantiate_runtime_plan(GenEditState p_edit_state) const {
	Ref<SceneInstantiationPlan> runtime_plan = _build_runtime_plan();
	if (runtime_plan.is_null() || runtime_plan->get_root_plan_id() < 0) {
		return _instantiate_legacy(p_edit_state);
	}
	const bool requires_legacy_fallback = _runtime_plan_requires_legacy_fallback(runtime_plan);
	const bool uses_customization = _runtime_plan_uses_customization(runtime_plan);
	if (requires_legacy_fallback || !uses_customization) {
		return _instantiate_legacy(p_edit_state);
	}

	Vector<DeferredNodePathProperties> deferred_node_paths;
	HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> resources_local_to_scenes;
	HashMap<int, ObjectID> materialized_plan_nodes;
	Node *root = _materialize_runtime_plan_node(runtime_plan, runtime_plan->get_root_plan_id(), nullptr, nullptr, nullptr, &deferred_node_paths, &resources_local_to_scenes, &materialized_plan_nodes, p_edit_state, true);
	if (!root) {
		return _instantiate_legacy(p_edit_state);
	}

	_resolve_runtime_plan_deferred_node_paths(deferred_node_paths);
	for (KeyValue<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &E : resources_local_to_scenes) {
		for (KeyValue<Ref<Resource>, Ref<Resource>> &R : E.value) {
			R.value->setup_local_to_scene();
		}
	}
	_apply_runtime_plan_connections(runtime_plan, materialized_plan_nodes, p_edit_state);

	return root;
}

Ref<SceneInstantiationPlan> SceneState::_build_runtime_plan() const {
	ERR_FAIL_COND_V(nodes.is_empty(), Ref<SceneInstantiationPlan>());

	Ref<SceneInstantiationPlan> runtime_plan;
	runtime_plan.instantiate();
	HashMap<const SceneState *, RuntimePlanSourceStateCache> source_state_caches;

	const int root_plan_id = _append_runtime_plan_node(runtime_plan, const_cast<SceneState *>(this), 0, -1, SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_LOCAL, source_state_caches);
	if (root_plan_id >= 0) {
		runtime_plan->set_root_plan_id(root_plan_id);
	}

	return runtime_plan;
}


const SceneState::RuntimePlanSourceStateCache *SceneState::_get_runtime_plan_source_state_cache(const Ref<SceneState> &p_source_state, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches) const {
	const SceneState *source_state = p_source_state.ptr();
	ERR_FAIL_NULL_V(source_state, nullptr);

	if (const RuntimePlanSourceStateCache *cache = r_source_state_caches.getptr(source_state)) {
		return cache;
	}

	RuntimePlanSourceStateCache cache;
	cache.node_paths.resize(p_source_state->nodes.size());
	cache.parent_paths.resize(p_source_state->nodes.size());
	cache.owner_paths.resize(p_source_state->nodes.size());
	cache.direct_children.resize(p_source_state->nodes.size());
	cache.override_children.resize(p_source_state->nodes.size());

	HashMap<NodePath, int> node_path_to_idx;
	for (int node_idx = 0; node_idx < p_source_state->nodes.size(); node_idx++) {
		cache.node_paths.write[node_idx] = p_source_state->get_node_path(node_idx);
		cache.parent_paths.write[node_idx] = _get_runtime_plan_parent_path(cache.node_paths[node_idx]);
		node_path_to_idx.insert(cache.node_paths[node_idx], node_idx);
	}

	for (int node_idx = 0; node_idx < p_source_state->nodes.size(); node_idx++) {
		const NodeData &node = p_source_state->nodes[node_idx];

		if (node.owner < 0 || node.owner == NO_PARENT_SAVED) {
			cache.owner_paths.write[node_idx] = NodePath();
		} else if (node.owner & FLAG_ID_IS_PATH) {
			const int owner_path_idx = node.owner & FLAG_MASK;
			if (owner_path_idx >= 0 && owner_path_idx < p_source_state->node_paths.size()) {
				cache.owner_paths.write[node_idx] = p_source_state->node_paths[owner_path_idx];
			} else {
				cache.owner_paths.write[node_idx] = NodePath();
			}
		} else {
			const int owner_node_idx = node.owner & FLAG_MASK;
			if (owner_node_idx >= 0 && owner_node_idx < cache.node_paths.size()) {
				cache.owner_paths.write[node_idx] = cache.node_paths[owner_node_idx];
			} else {
				cache.owner_paths.write[node_idx] = NodePath();
			}
		}

		if (node.parent < 0) {
			continue;
		}

		int parent_node_idx = -1;
		if (node.parent & FLAG_ID_IS_PATH) {
			const int parent_path_idx = node.parent & FLAG_MASK;
			if (parent_path_idx < 0 || parent_path_idx >= p_source_state->node_paths.size()) {
				continue;
			}

			const int *resolved_parent_node_idx = node_path_to_idx.getptr(p_source_state->node_paths[parent_path_idx]);
			if (resolved_parent_node_idx == nullptr) {
				continue;
			}
			parent_node_idx = *resolved_parent_node_idx;
		} else {
			parent_node_idx = node.parent & FLAG_MASK;
		}

		if (parent_node_idx < 0 || parent_node_idx >= cache.direct_children.size()) {
			continue;
		}

		cache.direct_children.write[parent_node_idx].push_back(node_idx);
	}

	for (int node_idx = 0; node_idx < cache.parent_paths.size(); node_idx++) {
		NodePath ancestor_path = cache.parent_paths[node_idx];
		while (!ancestor_path.is_empty()) {
			const int *ancestor_node_idx = node_path_to_idx.getptr(ancestor_path);
			if (ancestor_node_idx != nullptr) {
				cache.override_children.write[*ancestor_node_idx].push_back(node_idx);
				break;
			}
			ancestor_path = _get_runtime_plan_parent_path(ancestor_path);
		}
	}

	r_source_state_caches.insert(source_state, cache);
	return r_source_state_caches.getptr(source_state);
}

NodePath SceneState::_get_runtime_plan_node_path(const Ref<SceneState> &p_source_state, int p_source_node_idx, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches, bool p_for_parent) const {
	NodePath cached_path;
	ERR_FAIL_COND_V(p_source_state.is_null(), cached_path);
	const RuntimePlanSourceStateCache *cache = _get_runtime_plan_source_state_cache(p_source_state, r_source_state_caches);
	ERR_FAIL_NULL_V(cache, cached_path);
	if (p_for_parent) {
		ERR_FAIL_INDEX_V(p_source_node_idx, cache->parent_paths.size(), cached_path);
		return cache->parent_paths[p_source_node_idx];
	}
	ERR_FAIL_INDEX_V(p_source_node_idx, cache->node_paths.size(), cached_path);
	return cache->node_paths[p_source_node_idx];
}

NodePath SceneState::_get_runtime_plan_owner_path(const Ref<SceneState> &p_source_state, int p_source_node_idx, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches) const {
	NodePath cached_path;
	ERR_FAIL_COND_V(p_source_state.is_null(), cached_path);
	const RuntimePlanSourceStateCache *cache = _get_runtime_plan_source_state_cache(p_source_state, r_source_state_caches);
	ERR_FAIL_NULL_V(cache, cached_path);
	ERR_FAIL_INDEX_V(p_source_node_idx, cache->owner_paths.size(), cached_path);
	return cache->owner_paths[p_source_node_idx];
}

Vector<int> SceneState::_get_runtime_plan_direct_children(const Ref<SceneState> &p_source_state, int p_source_node_idx, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches) const {
	Vector<int> child_indices;
	ERR_FAIL_COND_V(p_source_state.is_null(), child_indices);
	const RuntimePlanSourceStateCache *cache = _get_runtime_plan_source_state_cache(p_source_state, r_source_state_caches);
	ERR_FAIL_NULL_V(cache, child_indices);
	ERR_FAIL_INDEX_V(p_source_node_idx, cache->direct_children.size(), child_indices);
	return cache->direct_children[p_source_node_idx];
}

bool SceneState::_runtime_plan_path_is_descendant(const NodePath &p_ancestor_path, const NodePath &p_descendant_path) const {
	if (p_descendant_path.is_empty() || p_descendant_path == NodePath(".")) {
		return false;
	}

	if (p_ancestor_path.is_empty() || p_ancestor_path == NodePath(".")) {
		return true;
	}

	const int ancestor_name_count = p_ancestor_path.get_name_count();
	if (p_descendant_path.get_name_count() <= ancestor_name_count) {
		return false;
	}

	for (int name_idx = 0; name_idx < ancestor_name_count; name_idx++) {
		if (p_ancestor_path.get_name(name_idx) != p_descendant_path.get_name(name_idx)) {
			return false;
		}
	}

	return true;
}

NodePath SceneState::_get_runtime_plan_parent_path(const NodePath &p_path) const {
	if (p_path.is_empty() || p_path == NodePath(".")) {
		return NodePath();
	}

	const int name_count = p_path.get_name_count();
	if (name_count <= 1) {
		return NodePath(".");
	}

	Vector<StringName> parent_names;
	parent_names.resize(name_count - 1);
	for (int name_idx = 0; name_idx < name_count - 1; name_idx++) {
		parent_names.write[name_idx] = p_path.get_name(name_idx);
	}

	return NodePath(parent_names, false);
}

Vector<int> SceneState::_get_runtime_plan_override_children(const Ref<SceneState> &p_source_state, int p_source_node_idx, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches) const {
	Vector<int> child_indices;
	ERR_FAIL_COND_V(p_source_state.is_null(), child_indices);
	const RuntimePlanSourceStateCache *cache = _get_runtime_plan_source_state_cache(p_source_state, r_source_state_caches);
	ERR_FAIL_NULL_V(cache, child_indices);
	ERR_FAIL_INDEX_V(p_source_node_idx, cache->override_children.size(), child_indices);
	return cache->override_children[p_source_node_idx];
}

NodePath SceneState::_compose_runtime_plan_path(const NodePath &p_base_path, const NodePath &p_relative_path) const {
	if (p_base_path.is_empty() || p_base_path == NodePath(".")) {
		return p_relative_path;
	}
	if (p_relative_path.is_empty() || p_relative_path == NodePath(".")) {
		return p_base_path;
	}

	Vector<StringName> path_names;
	for (int i = 0; i < p_base_path.get_name_count(); i++) {
		path_names.push_back(p_base_path.get_name(i));
	}
	for (int i = 0; i < p_relative_path.get_name_count(); i++) {
		const StringName name = p_relative_path.get_name(i);
		if (i == 0 && name == StringName(".")) {
			continue;
		}
		path_names.push_back(name);
	}

	if (path_names.is_empty()) {
		return NodePath(".");
	}

	return NodePath(path_names, false);
}

NodePath SceneState::_rebase_runtime_plan_subtree_path(const NodePath &p_root_source_path, const NodePath &p_path, bool p_fallback_to_root) const {
	if (p_path.is_empty()) {
		return NodePath();
	}

	if (p_root_source_path.is_empty() || p_root_source_path == NodePath(".")) {
		return p_path;
	}

	if (p_path == p_root_source_path) {
		return NodePath(".");
	}

	const String root_source_path = p_root_source_path.operator String();
	const String node_path_string = p_path.operator String();
	if (node_path_string.begins_with(root_source_path + "/")) {
		return NodePath("." + node_path_string.substr(root_source_path.length()));
	}

	return p_fallback_to_root ? NodePath(".") : NodePath();
}

static void _normalize_extracted_scene_owners(Node *p_root, Node *p_node) {
	ERR_FAIL_NULL(p_root);
	ERR_FAIL_NULL(p_node);

	for (int child_idx = 0; child_idx < p_node->get_child_count(); child_idx++) {
		Node *child = p_node->get_child(child_idx);
		ERR_CONTINUE(child == nullptr);
		child->set_owner(p_root);
		_normalize_extracted_scene_owners(p_root, child);
	}
}

int SceneState::_clone_runtime_plan_subtree(const Ref<SceneInstantiationPlan> &p_source_plan, int p_source_plan_id, const Ref<SceneInstantiationPlan> &p_target_plan, int p_target_parent_plan_id, const NodePath &p_root_source_path) const {
	ERR_FAIL_COND_V(p_source_plan.is_null(), -1);
	ERR_FAIL_COND_V(p_target_plan.is_null(), -1);

	const SceneInstantiationPlan::PlanNodeData *source_node = p_source_plan->_get_node_data(p_source_plan_id);
	ERR_FAIL_NULL_V(source_node, -1);
	ERR_FAIL_COND_V(source_node->pruned || source_node->flattened, -1);

	SceneInstantiationPlan::PlanNodeData copy = *source_node;
	copy.plan_id = p_target_plan->plan_nodes.size();
	copy.parent_plan_id = p_target_parent_plan_id;
	copy.child_plan_ids.clear();
	copy.source_path = p_target_parent_plan_id < 0 ? NodePath(".") : _rebase_runtime_plan_subtree_path(p_root_source_path, source_node->source_path, false);
	copy.owner_path = p_target_parent_plan_id < 0 ? NodePath() : _rebase_runtime_plan_subtree_path(p_root_source_path, source_node->owner_path, true);

	const int new_plan_id = copy.plan_id;
	p_target_plan->plan_nodes.push_back(copy);
	p_target_plan->_index_source_path(new_plan_id, copy.source_path);

	const Vector<int> source_child_plan_ids = source_node->child_plan_ids;
	for (int child_plan_id : source_child_plan_ids) {
		const int duplicated_child_plan_id = _clone_runtime_plan_subtree(p_source_plan, child_plan_id, p_target_plan, new_plan_id, p_root_source_path);
		if (duplicated_child_plan_id >= 0) {
			p_target_plan->plan_nodes.write[new_plan_id].child_plan_ids.push_back(duplicated_child_plan_id);
		}
	}

	return new_plan_id;
}

Ref<PackedScene> SceneState::_extract_runtime_plan_scene(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id) const {
	ERR_FAIL_COND_V(p_runtime_plan.is_null(), Ref<PackedScene>());

	const SceneInstantiationPlan::PlanNodeData *source_plan_node = p_runtime_plan->_get_node_data(p_plan_id);
	ERR_FAIL_NULL_V(source_plan_node, Ref<PackedScene>());
	ERR_FAIL_COND_V(source_plan_node->pruned || source_plan_node->flattened, Ref<PackedScene>());
	ERR_FAIL_COND_V(source_plan_node->source_state.is_null(), Ref<PackedScene>());

	Vector<DeferredNodePathProperties> deferred_node_paths;
	HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> resources_local_to_scenes;
	HashMap<int, ObjectID> materialized_plan_nodes;
	Node *root = _materialize_runtime_plan_node(p_runtime_plan, p_plan_id, nullptr, nullptr, nullptr, &deferred_node_paths, &resources_local_to_scenes, &materialized_plan_nodes, GEN_EDIT_STATE_DISABLED, false);
	ERR_FAIL_NULL_V(root, Ref<PackedScene>());

	_resolve_runtime_plan_deferred_node_paths(deferred_node_paths);
	for (KeyValue<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &E : resources_local_to_scenes) {
		for (KeyValue<Ref<Resource>, Ref<Resource>> &R : E.value) {
			R.value->setup_local_to_scene();
		}
	}

	if (source_plan_node->source_node_idx != 0) {
		auto canonicalize_connection_path = [](const NodePath &p_path) -> NodePath {
			if (p_path.is_empty() || p_path.is_absolute() || (p_path.get_name_count() > 0 && p_path.get_name(0) == StringName("."))) {
				return p_path;
			}
			return NodePath("./" + String(p_path));
		};

		auto apply_connections_from_state = [&](const SceneState *p_connection_state, int p_connection_root_idx) {
			if (p_connection_state == nullptr || p_connection_state->connections.is_empty()) {
				return;
			}

			for (const ConnectionData &connection : p_connection_state->connections) {
				NodePath from_path;
				if (connection.from & FLAG_ID_IS_PATH) {
					from_path = p_connection_state->node_paths[connection.from & FLAG_MASK];
				} else {
					from_path = p_connection_state->get_node_path(connection.from);
				}
				from_path = canonicalize_connection_path(from_path);

				NodePath to_path;
				if (connection.to & FLAG_ID_IS_PATH) {
					to_path = p_connection_state->node_paths[connection.to & FLAG_MASK];
				} else {
					to_path = p_connection_state->get_node_path(connection.to);
				}
				to_path = canonicalize_connection_path(to_path);

				const NodePath full_from_path = p_connection_root_idx == 0 ? _compose_runtime_plan_path(source_plan_node->source_path, from_path) : from_path;
				const NodePath full_to_path = p_connection_root_idx == 0 ? _compose_runtime_plan_path(source_plan_node->source_path, to_path) : to_path;
				const NodePath rebased_from_path = _rebase_runtime_plan_subtree_path(source_plan_node->source_path, full_from_path, false);
				const NodePath rebased_to_path = _rebase_runtime_plan_subtree_path(source_plan_node->source_path, full_to_path, false);
				if (rebased_from_path.is_empty() || rebased_to_path.is_empty()) {
					continue;
				}

				const int from_plan_id = _find_runtime_plan_node_by_source_path(p_runtime_plan, full_from_path);
				const int to_plan_id = _find_runtime_plan_node_by_source_path(p_runtime_plan, full_to_path);
				if (from_plan_id < 0 || to_plan_id < 0 || !materialized_plan_nodes.has(from_plan_id) || !materialized_plan_nodes.has(to_plan_id)) {
					continue;
				}

				Node *from_node = ObjectDB::get_instance<Node>(materialized_plan_nodes[from_plan_id]);
				Node *to_node = ObjectDB::get_instance<Node>(materialized_plan_nodes[to_plan_id]);
				if (!from_node || !to_node) {
					continue;
				}

				Callable callable(to_node, p_connection_state->names[connection.method]);
				Array binds;
				for (int bind : connection.binds) {
					binds.push_back(p_connection_state->variants[bind]);
				}

				if (!binds.is_empty()) {
					callable = callable.bindv(binds);
				}

				if (connection.unbinds > 0) {
					callable = callable.unbind(connection.unbinds);
				}

				if (from_node->is_connected(p_connection_state->names[connection.signal], callable)) {
					continue;
				}

				from_node->connect(p_connection_state->names[connection.signal], callable, CONNECT_PERSIST | connection.flags);
			}
		};

		apply_connections_from_state(source_plan_node->source_state.ptr(), source_plan_node->source_node_idx);
		for (const SceneState::PackState &override_source : source_plan_node->override_sources) {
			if (override_source.state.is_valid()) {
				apply_connections_from_state(override_source.state.ptr(), override_source.node);
			}
		}
	}

	_apply_runtime_plan_connections(p_runtime_plan, materialized_plan_nodes, GEN_EDIT_STATE_DISABLED);
	_normalize_extracted_scene_owners(root, root);

	Ref<PackedScene> extracted_scene;
	extracted_scene.instantiate();
	const Error err = extracted_scene->pack(root);
	memdelete(root);
	ERR_FAIL_COND_V(err != OK, Ref<PackedScene>());

	return extracted_scene;
}

void SceneState::_copy_runtime_plan_base_properties(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, const Ref<SceneState> &p_source_state, int p_source_node_idx) const {
	ERR_FAIL_COND(p_runtime_plan.is_null());
	ERR_FAIL_COND(p_source_state.is_null());
	ERR_FAIL_INDEX(p_source_node_idx, p_source_state->nodes.size());

	const NodeData &node = p_source_state->nodes[p_source_node_idx];
	for (int prop_idx = 0; prop_idx < node.properties.size(); prop_idx++) {
		const NodeData::Property &property = node.properties[prop_idx];
		const int property_name_idx = property.name & FLAG_PROP_NAME_MASK;
		if (property_name_idx < 0 || property_name_idx >= p_source_state->names.size() || property.value < 0 || property.value >= p_source_state->variants.size()) {
			continue;
		}
		const bool deferred_node_path = (property.name & FLAG_PATH_PROPERTY_IS_NODE) != 0;
		p_runtime_plan->set_node_base_property(p_plan_id, p_source_state->names[property_name_idx], p_source_state->variants[property.value], deferred_node_path);
	}
}

void SceneState::_apply_runtime_plan_property_overrides(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, const Ref<SceneState> &p_override_state, int p_override_node_idx) const {
	ERR_FAIL_COND(p_runtime_plan.is_null());
	ERR_FAIL_COND(p_override_state.is_null());
	ERR_FAIL_INDEX(p_override_node_idx, p_override_state->nodes.size());

	const NodeData &node = p_override_state->nodes[p_override_node_idx];
	for (int prop_idx = 0; prop_idx < node.properties.size(); prop_idx++) {
		const NodeData::Property &property = node.properties[prop_idx];
		const int property_name_idx = property.name & FLAG_PROP_NAME_MASK;
		if (property_name_idx < 0 || property_name_idx >= p_override_state->names.size() || property.value < 0 || property.value >= p_override_state->variants.size()) {
			continue;
		}
		p_runtime_plan->_set_node_property(p_plan_id, p_override_state->names[property_name_idx], p_override_state->variants[property.value], property.name & FLAG_PATH_PROPERTY_IS_NODE);
	}
}

void SceneState::_resolve_runtime_plan_deferred_node_paths(const Vector<DeferredNodePathProperties> &p_deferred_node_paths) const {
	for (const DeferredNodePathProperties &dnp : p_deferred_node_paths) {
		Node *base = ObjectDB::get_instance<Node>(dnp.base);
		ERR_CONTINUE_EDMSG(!base, vformat("Failed to set deferred property '%s' as the base node disappeared.", dnp.property));
		if (dnp.value.get_type() == Variant::ARRAY) {
			Array paths = dnp.value;

			bool valid;
			Array array = base->get(dnp.property, &valid);
			ERR_CONTINUE_EDMSG(!valid, vformat("Failed to get property '%s' from node '%s'.", dnp.property, base->get_name()));
			array = array.duplicate();

			array.resize(paths.size());
			for (int i = 0; i < array.size(); i++) {
				array.set(i, base->get_node_or_null(paths[i]));
			}
			base->set(dnp.property, array);
		} else if (dnp.value.get_type() == Variant::DICTIONARY) {
			Dictionary paths = dnp.value;

			bool valid;
			Dictionary dict = base->get(dnp.property, &valid);
			ERR_CONTINUE_EDMSG(!valid, vformat("Failed to get property '%s' from node '%s'.", dnp.property, base->get_name()));
			dict = dict.duplicate();
			bool convert_key = dict.get_typed_key_builtin() == Variant::OBJECT &&
					ClassDB::is_parent_class(dict.get_typed_key_class_name(), "Node");
			bool convert_value = dict.get_typed_value_builtin() == Variant::OBJECT &&
					ClassDB::is_parent_class(dict.get_typed_value_class_name(), "Node");

			for (const KeyValue<Variant, Variant> &kv : paths) {
				Variant key = kv.key;
				if (convert_key) {
					key = base->get_node_or_null(key);
				}
				Variant value = kv.value;
				if (convert_value) {
					value = base->get_node_or_null(value);
				}
				dict[key] = value;
			}
			base->set(dnp.property, dict);
		} else {
			base->set(dnp.property, base->get_node_or_null(dnp.value));
		}
	}
}

Node *SceneState::_get_runtime_plan_local_resource_base(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, Node *p_node, Node *p_root, Node *p_source_root) const {
	ERR_FAIL_COND_V(p_runtime_plan.is_null(), nullptr);
	ERR_FAIL_NULL_V(p_node, nullptr);
	const SceneInstantiationPlan::PlanNodeData *plan_node = p_runtime_plan->_get_node_data(p_plan_id);
	ERR_FAIL_NULL_V(plan_node, nullptr);

	if (plan_node->instance_root || plan_node->source_node_idx == 0) {
		return p_node;
	}

	const NodePath owner_path = plan_node->owner_path;
	if (owner_path == NodePath(".")) {
		return p_source_root ? p_source_root : p_node;
	}
	if (!owner_path.is_empty() && p_source_root) {
		Node *owner = p_source_root->get_node_or_null(owner_path);
		if (owner) {
			return owner;
		}
	}

	return p_root ? p_root : p_node;
}

Variant SceneState::_make_runtime_plan_local_resource(Variant &p_value, const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_property_name, Node *p_root, Node *p_source_root, GenEditState p_edit_state) const {
	ERR_FAIL_COND_V(p_runtime_plan.is_null(), p_value);
	const SceneInstantiationPlan::PlanNodeData *plan_node = p_runtime_plan->_get_node_data(p_plan_id);
	ERR_FAIL_NULL_V(plan_node, p_value);

	Ref<Resource> resource = p_value;
	if (resource.is_null() || !resource->is_local_to_scene()) {
		return p_value;
	}

	Node *base = _get_runtime_plan_local_resource_base(p_runtime_plan, p_plan_id, p_node, p_root, p_source_root);
	ERR_FAIL_NULL_V(base, p_value);

	if (plan_node->instance_root) {
		return get_remap_resource(resource, p_resources_local_to_scenes, p_node->get(p_property_name), base);
	}

	HashMap<Ref<Resource>, Ref<Resource>>::Iterator remapped_resource = p_resources_local_to_scenes[base].find(resource);
	if (remapped_resource) {
		return remapped_resource->value;
	}

	if (p_edit_state == GEN_EDIT_STATE_MAIN) {
		resource->configure_for_local_scene(base, p_resources_local_to_scenes[base]);
		p_resources_local_to_scenes[base][resource] = resource;
		return resource;
	}

	Ref<Resource> local_duplicate = resource->duplicate_for_local_scene(base, p_resources_local_to_scenes[base]);
	p_resources_local_to_scenes[base][resource] = local_duplicate;
	return local_duplicate;
}

Array SceneState::_setup_runtime_plan_resources_in_array(Array &p_array_to_scan, const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_property_name, Node *p_root, Node *p_source_root, GenEditState p_edit_state) const {
	for (int i = 0; i < p_array_to_scan.size(); i++) {
		if (p_array_to_scan[i].get_type() == Variant::OBJECT) {
			p_array_to_scan[i] = _make_runtime_plan_local_resource(p_array_to_scan[i], p_runtime_plan, p_plan_id, p_resources_local_to_scenes, p_node, p_property_name, p_root, p_source_root, p_edit_state);
		}
	}
	return p_array_to_scan;
}

Dictionary SceneState::_setup_runtime_plan_resources_in_dictionary(Dictionary &p_dictionary_to_scan, const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_property_name, Node *p_root, Node *p_source_root, GenEditState p_edit_state) const {
	const Array keys = p_dictionary_to_scan.keys();
	const Array values = p_dictionary_to_scan.values();

	if (has_local_resource(values) || has_local_resource(keys)) {
		Array duplicated_keys = keys.duplicate(true);
		Array duplicated_values = values.duplicate(true);

		duplicated_keys = _setup_runtime_plan_resources_in_array(duplicated_keys, p_runtime_plan, p_plan_id, p_resources_local_to_scenes, p_node, p_property_name, p_root, p_source_root, p_edit_state);
		duplicated_values = _setup_runtime_plan_resources_in_array(duplicated_values, p_runtime_plan, p_plan_id, p_resources_local_to_scenes, p_node, p_property_name, p_root, p_source_root, p_edit_state);
		p_dictionary_to_scan.clear();

		for (int i = 0; i < keys.size(); i++) {
			p_dictionary_to_scan[duplicated_keys[i]] = duplicated_values[i];
		}
	}

	return p_dictionary_to_scan;
}

int SceneState::_find_runtime_plan_node_by_source_path(const Ref<SceneInstantiationPlan> &p_runtime_plan, const NodePath &p_source_path) const {
	ERR_FAIL_COND_V(p_runtime_plan.is_null(), -1);
	const HashSet<int> *indexed_plan_ids = p_runtime_plan->source_path_index.getptr(p_source_path);
	if (indexed_plan_ids == nullptr || indexed_plan_ids->size() == 0) {
		return -1;
	}
	if (indexed_plan_ids->size() > 1) {
		int non_duplicated_plan_id = -1;
		for (const int &indexed_plan_id : *indexed_plan_ids) {
			const SceneInstantiationPlan::PlanNodeData *plan_node = p_runtime_plan->_get_node_data(indexed_plan_id);
			if (plan_node == nullptr || plan_node->origin == SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_DUPLICATED) {
				continue;
			}
			if (non_duplicated_plan_id >= 0) {
				return -1;
			}
			non_duplicated_plan_id = indexed_plan_id;
		}
		if (non_duplicated_plan_id >= 0) {
			return non_duplicated_plan_id;
		}
		return -1;
	}

	int plan_id = -1;
	for (const int &indexed_plan_id : *indexed_plan_ids) {
		plan_id = indexed_plan_id;
		break;
	}

	return plan_id;
}

void SceneState::_merge_runtime_plan_instance_overrides(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, const Ref<SceneState> &p_override_state, int p_override_node_idx, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches) const {
	ERR_FAIL_COND(p_runtime_plan.is_null());
	ERR_FAIL_COND(p_override_state.is_null());
	ERR_FAIL_INDEX(p_override_node_idx, p_override_state->nodes.size());
	const SceneInstantiationPlan::PlanNodeData *plan_node = p_runtime_plan->_get_node_data(p_plan_id);
	ERR_FAIL_NULL(plan_node);
	const NodePath plan_source_path = plan_node->source_path;

	const NodePath override_root_path = _get_runtime_plan_node_path(p_override_state, p_override_node_idx, r_source_state_caches);

	const Vector<int> override_children = _get_runtime_plan_override_children(p_override_state, p_override_node_idx, r_source_state_caches);
	for (int child_node_idx : override_children) {
		const NodeData &override_child = p_override_state->nodes[child_node_idx];
		const NodePath override_child_path = _get_runtime_plan_node_path(p_override_state, child_node_idx, r_source_state_caches);
		NodePath relative_child_path = override_child_path;
		if (!override_root_path.is_empty() && override_root_path != NodePath(".")) {
			const String override_root_path_string = override_root_path.operator String();
			const String override_child_path_string = override_child_path.operator String();
			if (override_child_path_string.begins_with(override_root_path_string + "/")) {
				relative_child_path = NodePath("." + override_child_path_string.substr(override_root_path_string.length()));
			}
		}
		const NodePath child_path = _compose_runtime_plan_path(plan_source_path, relative_child_path);

		NodePath override_parent_path = _get_runtime_plan_node_path(p_override_state, child_node_idx, r_source_state_caches, true);
		NodePath relative_parent_path = override_parent_path;
		if (!override_root_path.is_empty() && override_root_path != NodePath(".")) {
			const String override_root_path_string = override_root_path.operator String();
			const String override_parent_path_string = override_parent_path.operator String();
			if (override_parent_path == override_root_path) {
				relative_parent_path = NodePath(".");
			} else if (override_parent_path_string.begins_with(override_root_path_string + "/")) {
				relative_parent_path = NodePath("." + override_parent_path_string.substr(override_root_path_string.length()));
			}
		}
		const NodePath parent_path = _compose_runtime_plan_path(plan_source_path, relative_parent_path);
		int parent_plan_id = p_plan_id;
		const int matched_parent_plan_id = _find_runtime_plan_node_by_source_path(p_runtime_plan, parent_path);
		if (matched_parent_plan_id >= 0) {
			parent_plan_id = matched_parent_plan_id;
		}

		if (override_child.type == TYPE_INSTANTIATED) {
			const int target_plan_id = _find_runtime_plan_node_by_source_path(p_runtime_plan, child_path);
			if (target_plan_id < 0) {
				_append_runtime_plan_node(p_runtime_plan, p_override_state, child_node_idx, parent_plan_id, SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_OWNER_ADDED, r_source_state_caches, child_path);
				continue;
			}
			_apply_runtime_plan_property_overrides(p_runtime_plan, target_plan_id, p_override_state, child_node_idx);
			_merge_runtime_plan_instance_overrides(p_runtime_plan, target_plan_id, p_override_state, child_node_idx, r_source_state_caches);
			continue;
		}

		_append_runtime_plan_node(p_runtime_plan, p_override_state, child_node_idx, parent_plan_id, SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_OWNER_ADDED, r_source_state_caches, child_path);
	}
}

int SceneState::_append_runtime_plan_node(const Ref<SceneInstantiationPlan> &p_runtime_plan, const Ref<SceneState> &p_source_state, int p_source_node_idx, int p_parent_plan_id, SceneInstantiationPlanNodeOrigin p_origin, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches, const NodePath &p_source_path_override, const StringName &p_name_override) const {
	ERR_FAIL_COND_V(p_runtime_plan.is_null(), -1);
	ERR_FAIL_COND_V(p_source_state.is_null(), -1);
	ERR_FAIL_INDEX_V(p_source_node_idx, p_source_state->nodes.size(), -1);

	const NodeData &source_node = p_source_state->nodes[p_source_node_idx];
	const NodePath source_path = p_source_path_override.is_empty() ? _get_runtime_plan_node_path(p_source_state, p_source_node_idx, r_source_state_caches) : p_source_path_override;
	const NodePath owner_path = _get_runtime_plan_owner_path(p_source_state, p_source_node_idx, r_source_state_caches);
	Ref<PackedScene> instance_scene = p_source_state->get_node_instance(p_source_node_idx);

	if (instance_scene.is_valid()) {
		Ref<SceneState> instance_state = instance_scene->get_state();
		if (instance_state.is_valid() && instance_state->get_node_count() > 0) {
			const StringName merged_name = p_name_override == StringName() ? p_source_state->get_node_name(p_source_node_idx) : p_name_override;
			const int plan_id = _append_runtime_plan_node(p_runtime_plan, instance_state, 0, p_parent_plan_id, SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_NESTED_SCENE, r_source_state_caches, source_path, merged_name);
			SceneInstantiationPlan::PlanNodeData *plan_node = p_runtime_plan->_get_node_data_w(plan_id);
			ERR_FAIL_NULL_V(plan_node, plan_id);
			plan_node->owner_path = owner_path;
			if (p_source_node_idx == 0) {
				SceneState::PackState override_source;
				override_source.state = p_source_state;
				override_source.node = p_source_node_idx;
				plan_node->override_sources.push_back(override_source);
			}
			_apply_runtime_plan_property_overrides(p_runtime_plan, plan_id, p_source_state, p_source_node_idx);

			_merge_runtime_plan_instance_overrides(p_runtime_plan, plan_id, p_source_state, p_source_node_idx, r_source_state_caches);

			return plan_id;
		}
	}

	const StringName node_name = p_name_override == StringName() ? p_source_state->get_node_name(p_source_node_idx) : p_name_override;
	StringName node_type = p_source_state->get_node_type(p_source_node_idx);
	if (node_type == StringName() && source_node.type == TYPE_INSTANTIATED) {
		node_type = StringName("TYPE_INSTANTIATED");
	}

	const int plan_id = p_runtime_plan->add_node(p_source_state, p_source_node_idx, p_parent_plan_id, source_path, p_source_state->get_path(), node_name, node_type, p_origin, false);
	SceneInstantiationPlan::PlanNodeData *plan_node = p_runtime_plan->_get_node_data_w(plan_id);
	ERR_FAIL_NULL_V(plan_node, plan_id);
	plan_node->owner_path = owner_path;
	_copy_runtime_plan_base_properties(p_runtime_plan, plan_id, p_source_state, p_source_node_idx);

	const NodePath current_local_source_path = _get_runtime_plan_node_path(p_source_state, p_source_node_idx, r_source_state_caches);
	const Vector<int> child_nodes = _get_runtime_plan_direct_children(p_source_state, p_source_node_idx, r_source_state_caches);
	for (int child_node_idx : child_nodes) {
		const NodePath source_child_path = _get_runtime_plan_node_path(p_source_state, child_node_idx, r_source_state_caches);
		NodePath relative_child_path = source_child_path;
		if (!current_local_source_path.is_empty() && current_local_source_path != NodePath(".")) {
			const String current_local_source_path_string = current_local_source_path.operator String();
			const String source_child_path_string = source_child_path.operator String();
			if (source_child_path_string.begins_with(current_local_source_path_string + "/")) {
				relative_child_path = NodePath("." + source_child_path_string.substr(current_local_source_path_string.length()));
			}
		}
		const NodePath child_path = _compose_runtime_plan_path(source_path, relative_child_path);
		_append_runtime_plan_node(p_runtime_plan, p_source_state, child_node_idx, plan_id, p_origin, r_source_state_caches, child_path);
	}

	return plan_id;
}

bool SceneState::_runtime_plan_requires_legacy_fallback(const Ref<SceneInstantiationPlan> &p_runtime_plan) const {
	ERR_FAIL_COND_V(p_runtime_plan.is_null(), true);

	if (_runtime_plan_requires_legacy_connection_fallback(p_runtime_plan)) {
		return true;
	}

	for (const SceneInstantiationPlan::PlanNodeData &plan_node : p_runtime_plan->plan_nodes) {
		if (plan_node.pruned) {
			continue;
		}
		if (plan_node.source_state.is_null()) {
			return true;
		}
		const SceneState *source_state = plan_node.source_state.ptr();
		if (plan_node.source_node_idx < 0 || plan_node.source_node_idx >= source_state->nodes.size()) {
			return true;
		}

		const NodeData &source_node = source_state->nodes[plan_node.source_node_idx];
		// Expanded instance roots and TYPE_INSTANTIATED override stubs are already
		// represented in the runtime plan. Only unresolved scene-instance records,
		// such as placeholders that could not be expanded into plan nodes, still
		// need the legacy instantiate path.
		if (source_node.instance >= 0 && !plan_node.instance_root) {
			return true;
		}

		for (const NodeData::Property &property : source_node.properties) {
			const int property_name_idx = property.name & FLAG_PROP_NAME_MASK;
			if (property_name_idx < 0 || property_name_idx >= source_state->names.size()) {
				continue;
			}

			const StringName property_name = source_state->names[property_name_idx];
			if (!plan_node.properties.has(property_name)) {
				continue;
			}

			const Variant &value = plan_node.properties[property_name];
			if (value.get_type() != Variant::OBJECT) {
				continue;
			}
		}
	}

	return false;
}

bool SceneState::_runtime_plan_requires_legacy_connection_fallback(const Ref<SceneInstantiationPlan> &p_runtime_plan) const {
	ERR_FAIL_COND_V(p_runtime_plan.is_null(), true);

	bool has_connections = false;
	for (const SceneInstantiationPlan::PlanNodeData &plan_node : p_runtime_plan->plan_nodes) {
		if (plan_node.pruned || plan_node.flattened || plan_node.source_state.is_null() || plan_node.source_node_idx != 0) {
			continue;
		}
		if (!plan_node.source_state->connections.is_empty()) {
			has_connections = true;
			break;
		}
		for (const SceneState::PackState &override_source : plan_node.override_sources) {
			if (override_source.state.is_valid() && !override_source.state->connections.is_empty()) {
				has_connections = true;
				break;
			}
		}
		if (has_connections) {
			has_connections = true;
			break;
		}
	}

	if (!has_connections) {
		return false;
	}

	for (const SceneInstantiationPlan::PlanNodeData &plan_node : p_runtime_plan->plan_nodes) {
		if (plan_node.flattened || plan_node.origin == SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_DUPLICATED) {
			return true;
		}
	}

	return false;
}

void SceneState::_apply_runtime_plan_connections(const Ref<SceneInstantiationPlan> &p_runtime_plan, const HashMap<int, ObjectID> &p_materialized_plan_nodes, GenEditState p_edit_state) const {
	ERR_FAIL_COND(p_runtime_plan.is_null());
	auto get_plan_node = [&](int p_plan_id) -> const SceneInstantiationPlan::PlanNodeData * {
		if (!p_runtime_plan->has_node(p_plan_id)) {
			return nullptr;
		}
		return p_runtime_plan->_get_node_data(p_plan_id);
	};
	auto is_duplicate_root = [&](int p_plan_id) -> bool {
		const SceneInstantiationPlan::PlanNodeData *plan_node = get_plan_node(p_plan_id);
		if (plan_node == nullptr || plan_node->origin != SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_DUPLICATED || plan_node->duplicated_from_plan_id < 0) {
			return false;
		}

		const SceneInstantiationPlan::PlanNodeData *source_plan_node = get_plan_node(plan_node->duplicated_from_plan_id);
		if (source_plan_node == nullptr) {
			return false;
		}

		const SceneInstantiationPlan::PlanNodeData *parent_plan_node = get_plan_node(plan_node->parent_plan_id);
		return parent_plan_node == nullptr || parent_plan_node->duplicated_from_plan_id != source_plan_node->parent_plan_id;
	};
	auto is_plan_descendant_or_same = [&](int p_ancestor_plan_id, int p_plan_id) -> bool {
		int current_plan_id = p_plan_id;
		while (current_plan_id >= 0) {
			if (current_plan_id == p_ancestor_plan_id) {
				return true;
			}

			const SceneInstantiationPlan::PlanNodeData *plan_node = get_plan_node(current_plan_id);
			if (plan_node == nullptr) {
				break;
			}

			current_plan_id = plan_node->parent_plan_id;
		}

		return false;
	};
	auto map_plan_id_into_duplicate_root = [&](int p_source_plan_id, int p_duplicate_root_plan_id) -> int {
		const SceneInstantiationPlan::PlanNodeData *duplicate_root_node = get_plan_node(p_duplicate_root_plan_id);
		if (duplicate_root_node == nullptr || duplicate_root_node->duplicated_from_plan_id < 0) {
			return -1;
		}

		const int source_root_plan_id = duplicate_root_node->duplicated_from_plan_id;
		if (!is_plan_descendant_or_same(source_root_plan_id, p_source_plan_id)) {
			return p_source_plan_id;
		}

		if (p_source_plan_id == source_root_plan_id) {
			return p_duplicate_root_plan_id;
		}

		Vector<int> source_plan_path;
		int current_source_plan_id = p_source_plan_id;
		while (current_source_plan_id >= 0 && current_source_plan_id != source_root_plan_id) {
			source_plan_path.push_back(current_source_plan_id);
			const SceneInstantiationPlan::PlanNodeData *current_source_plan_node = get_plan_node(current_source_plan_id);
			if (current_source_plan_node == nullptr) {
				return -1;
			}
			current_source_plan_id = current_source_plan_node->parent_plan_id;
		}

		if (current_source_plan_id != source_root_plan_id) {
			return -1;
		}

		int current_duplicate_plan_id = p_duplicate_root_plan_id;
		for (int path_idx = source_plan_path.size() - 1; path_idx >= 0; path_idx--) {
			const int target_source_plan_id = source_plan_path[path_idx];
			const SceneInstantiationPlan::PlanNodeData *current_duplicate_plan_node = get_plan_node(current_duplicate_plan_id);
			if (current_duplicate_plan_node == nullptr) {
				return -1;
			}

			int next_duplicate_plan_id = -1;
			for (int child_plan_id : current_duplicate_plan_node->child_plan_ids) {
				const SceneInstantiationPlan::PlanNodeData *child_plan_node = get_plan_node(child_plan_id);
				if (child_plan_node != nullptr && child_plan_node->duplicated_from_plan_id == target_source_plan_id) {
					next_duplicate_plan_id = child_plan_id;
					break;
				}
			}

			if (next_duplicate_plan_id < 0) {
				return -1;
			}

			current_duplicate_plan_id = next_duplicate_plan_id;
		}

		return current_duplicate_plan_id;
	};
	Vector<int> duplicate_root_plan_ids;
	Vector<Vector<int>> duplicate_root_chains;
	for (const SceneInstantiationPlan::PlanNodeData &plan_node : p_runtime_plan->plan_nodes) {
		if (plan_node.pruned || plan_node.flattened || !is_duplicate_root(plan_node.plan_id) || !p_materialized_plan_nodes.has(plan_node.plan_id)) {
			continue;
		}

		Vector<int> reverse_duplicate_root_chain;
		int current_plan_id = plan_node.plan_id;
		while (current_plan_id >= 0) {
			if (is_duplicate_root(current_plan_id)) {
				reverse_duplicate_root_chain.push_back(current_plan_id);
			}

			const SceneInstantiationPlan::PlanNodeData *current_plan_node = get_plan_node(current_plan_id);
			if (current_plan_node == nullptr) {
				break;
			}

			current_plan_id = current_plan_node->parent_plan_id;
		}

		Vector<int> duplicate_root_chain;
		duplicate_root_chain.resize(reverse_duplicate_root_chain.size());
		for (int chain_idx = 0; chain_idx < reverse_duplicate_root_chain.size(); chain_idx++) {
			duplicate_root_chain.write[chain_idx] = reverse_duplicate_root_chain[reverse_duplicate_root_chain.size() - 1 - chain_idx];
		}

		duplicate_root_plan_ids.push_back(plan_node.plan_id);
		duplicate_root_chains.push_back(duplicate_root_chain);
	}
	auto canonicalize_connection_path = [](const NodePath &p_path) -> NodePath {
		if (p_path.is_empty() || p_path.is_absolute() || (p_path.get_name_count() > 0 && p_path.get_name(0) == StringName("."))) {
			return p_path;
		}
		return NodePath("./" + String(p_path));
	};
	auto connect_plan_nodes = [&](const SceneState *p_connection_state, const ConnectionData &p_connection, int p_from_plan_id, int p_to_plan_id) {
		if (p_from_plan_id < 0 || p_to_plan_id < 0 || !p_materialized_plan_nodes.has(p_from_plan_id) || !p_materialized_plan_nodes.has(p_to_plan_id)) {
			return;
		}

		Node *from_node = ObjectDB::get_instance<Node>(p_materialized_plan_nodes[p_from_plan_id]);
		Node *to_node = ObjectDB::get_instance<Node>(p_materialized_plan_nodes[p_to_plan_id]);
		if (!from_node || !to_node) {
			return;
		}

		Callable callable(to_node, p_connection_state->names[p_connection.method]);
		Array binds;
		for (int bind : p_connection.binds) {
			binds.push_back(p_connection_state->variants[bind]);
		}

		if (!binds.is_empty()) {
			callable = callable.bindv(binds);
		}

		if (p_connection.unbinds > 0) {
			callable = callable.unbind(p_connection.unbinds);
		}

		if (from_node->is_connected(p_connection_state->names[p_connection.signal], callable)) {
			return;
		}

		from_node->connect(p_connection_state->names[p_connection.signal], callable, CONNECT_PERSIST | p_connection.flags | (p_edit_state == GEN_EDIT_STATE_MAIN ? 0 : CONNECT_INHERITED));
	};
	auto apply_connections_from_state = [&](const SceneInstantiationPlan::PlanNodeData &p_scene_root_plan, const SceneState *p_connection_state) {
		if (p_connection_state == nullptr || p_connection_state->connections.is_empty()) {
			return;
		}

		for (const ConnectionData &connection : p_connection_state->connections) {
			NodePath from_path;
			if (connection.from & FLAG_ID_IS_PATH) {
				from_path = p_connection_state->node_paths[connection.from & FLAG_MASK];
			} else {
				from_path = p_connection_state->get_node_path(connection.from);
			}
			from_path = canonicalize_connection_path(from_path);

			NodePath to_path;
			if (connection.to & FLAG_ID_IS_PATH) {
				to_path = p_connection_state->node_paths[connection.to & FLAG_MASK];
			} else {
				to_path = p_connection_state->get_node_path(connection.to);
			}
			to_path = canonicalize_connection_path(to_path);

			const NodePath full_from_path = _compose_runtime_plan_path(p_scene_root_plan.source_path, from_path);
			const NodePath full_to_path = _compose_runtime_plan_path(p_scene_root_plan.source_path, to_path);

			const int from_plan_id = _find_runtime_plan_node_by_source_path(p_runtime_plan, full_from_path);
			const int to_plan_id = _find_runtime_plan_node_by_source_path(p_runtime_plan, full_to_path);
			connect_plan_nodes(p_connection_state, connection, from_plan_id, to_plan_id);

			for (int duplicate_root_idx = 0; duplicate_root_idx < duplicate_root_plan_ids.size(); duplicate_root_idx++) {
				int duplicate_from_plan_id = from_plan_id;
				int duplicate_to_plan_id = to_plan_id;

				for (int duplicate_root_plan_id : duplicate_root_chains[duplicate_root_idx]) {
					if (duplicate_from_plan_id >= 0) {
						duplicate_from_plan_id = map_plan_id_into_duplicate_root(duplicate_from_plan_id, duplicate_root_plan_id);
					}
					if (duplicate_to_plan_id >= 0) {
						duplicate_to_plan_id = map_plan_id_into_duplicate_root(duplicate_to_plan_id, duplicate_root_plan_id);
					}
				}

				if (duplicate_from_plan_id < 0 || duplicate_to_plan_id < 0) {
					continue;
				}

				if (duplicate_from_plan_id == from_plan_id && duplicate_to_plan_id == to_plan_id) {
					continue;
				}

				connect_plan_nodes(p_connection_state, connection, duplicate_from_plan_id, duplicate_to_plan_id);
			}
		}
	};

	for (const SceneInstantiationPlan::PlanNodeData &scene_root_plan : p_runtime_plan->plan_nodes) {
		if (scene_root_plan.pruned || scene_root_plan.flattened || scene_root_plan.source_state.is_null() || scene_root_plan.source_node_idx != 0) {
			continue;
		}

		apply_connections_from_state(scene_root_plan, scene_root_plan.source_state.ptr());
		for (const SceneState::PackState &override_source : scene_root_plan.override_sources) {
			if (override_source.state.is_valid()) {
				apply_connections_from_state(scene_root_plan, override_source.state.ptr());
			}
		}
	}
}

bool SceneState::_runtime_plan_uses_customization(const Ref<SceneInstantiationPlan> &p_runtime_plan) const {
	ERR_FAIL_COND_V(p_runtime_plan.is_null(), false);

	for (const SceneInstantiationPlan::PlanNodeData &plan_node : p_runtime_plan->plan_nodes) {
		if (plan_node.pruned) {
			continue;
		}

		if (plan_node.type != StringName()) {
			if (::ClassDB::has_method(plan_node.type, SNAME("_customize_scene_instantiation")) ||
					::ClassDB::has_method(plan_node.type, SNAME("_filter_scene_children"))) {
				return true;
			}
		}

		if (plan_node.properties.has(CoreStringName(script))) {
			Ref<Script> script = plan_node.properties[CoreStringName(script)];
			if (script.is_valid() && (script->has_method(SNAME("_customize_scene_instantiation")) || script->has_method(SNAME("_filter_scene_children")))) {
				return true;
			}
		}
	}

	return false;
}

void SceneState::_apply_legacy_filter_to_runtime_plan(Node *p_node, const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id) const {
	ERR_FAIL_NULL(p_node);
	ERR_FAIL_COND(p_runtime_plan.is_null());

	const SceneInstantiationPlan::PlanNodeData *plan_node = p_runtime_plan->_get_node_data(p_plan_id);
	ERR_FAIL_NULL(plan_node);

	const Vector<int> child_plan_ids = plan_node->child_plan_ids;
	Array child_infos;
	for (int child_plan_id : child_plan_ids) {
		const SceneInstantiationPlan::PlanNodeData *child_node = p_runtime_plan->_get_node_data(child_plan_id);
		if (!child_node || child_node->pruned) {
			continue;
		}

		Dictionary child_info;
		child_info["id"] = child_plan_id;
		child_info["name"] = child_node->name;
		child_info["type"] = child_node->type;

		Dictionary property_dict;
		for (const KeyValue<StringName, Variant> &kv : child_node->properties) {
			property_dict[kv.key] = kv.value;
		}
		child_info["properties"] = property_dict;
		child_infos.push_back(child_info);
	}

	Variant filter_result = p_node->call("_filter_scene_children", child_infos);
	if (filter_result.get_type() != Variant::ARRAY) {
		return;
	}

	HashMap<int, Vector<Dictionary>> filtered_entries_by_id;
	const Array filtered_infos = filter_result;
	for (int i = 0; i < filtered_infos.size(); i++) {
		Dictionary filtered_info = filtered_infos[i];
		if (!filtered_info.has("id")) {
			continue;
		}
		const int child_plan_id = filtered_info["id"];
		if (!p_runtime_plan->has_node(child_plan_id)) {
			continue;
		}
		if (!filtered_entries_by_id.has(child_plan_id)) {
			filtered_entries_by_id.insert(child_plan_id, Vector<Dictionary>());
		}
		filtered_entries_by_id[child_plan_id].push_back(filtered_info);
	}

	for (int child_plan_id : child_plan_ids) {
		if (!filtered_entries_by_id.has(child_plan_id)) {
			p_runtime_plan->_prune_node(child_plan_id);
			continue;
		}

		auto apply_filtered_entry = [&](const Ref<SceneInstantiationPlanNode> &p_plan_node_ref, const Dictionary &p_filtered_entry) {
			if (p_plan_node_ref.is_null() || !p_filtered_entry.has("properties")) {
				return;
			}

			Dictionary override_properties = p_filtered_entry["properties"];
			for (const KeyValue<Variant, Variant> &kv : override_properties) {
				const StringName property_name = kv.key;
				if (property_name == StringName("name")) {
					p_plan_node_ref->set_name(kv.value);
				} else {
					p_plan_node_ref->set_property(property_name, kv.value);
				}
			}
		};

		const Vector<Dictionary> &filtered_entries = filtered_entries_by_id[child_plan_id];
		apply_filtered_entry(p_runtime_plan->_make_node_ref(child_plan_id), filtered_entries[0]);

		if (filtered_entries.size() > 1) {
			const Array duplicates = p_runtime_plan->_duplicate_node(child_plan_id, filtered_entries.size() - 1);
			for (int duplicate_idx = 0; duplicate_idx < duplicates.size() && duplicate_idx + 1 < filtered_entries.size(); duplicate_idx++) {
				Ref<SceneInstantiationPlanNode> duplicate_node = duplicates[duplicate_idx];
				apply_filtered_entry(duplicate_node, filtered_entries[duplicate_idx + 1]);
			}
		}
	}
}

Node *SceneState::_materialize_runtime_plan_node(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, Node *p_parent, Node *p_root, Node *p_source_root, Vector<DeferredNodePathProperties> *p_deferred_node_paths, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> *p_resources_local_to_scenes, HashMap<int, ObjectID> *p_materialized_plan_nodes, GenEditState p_edit_state, bool p_apply_customization) const {
	ERR_FAIL_COND_V(p_runtime_plan.is_null(), nullptr);

	const SceneInstantiationPlan::PlanNodeData *plan_node = p_runtime_plan->_get_node_data(p_plan_id);
	ERR_FAIL_NULL_V(plan_node, nullptr);
	if (plan_node->pruned) {
		return nullptr;
	}

	ERR_FAIL_COND_V(plan_node->source_state.is_null(), nullptr);
	const SceneState *source_state = plan_node->source_state.ptr();
	ERR_FAIL_COND_V(plan_node->source_node_idx < 0 || plan_node->source_node_idx >= source_state->nodes.size(), nullptr);
	const NodeData &source_node = source_state->nodes[plan_node->source_node_idx];

	Object *obj = ::ClassDB::instantiate(plan_node->type);
	Node *node = Object::cast_to<Node>(obj);
	if (!node) {
		if (obj) {
			memdelete(obj);
			obj = nullptr;
		}

		if (ResourceLoader::is_creating_missing_resources_if_class_unavailable_enabled()) {
			MissingNode *missing_node = memnew(MissingNode);
			if (plan_node->type != StringName()) {
				missing_node->set_original_class(plan_node->type);
			}
			missing_node->set_recording_properties(true);
			node = missing_node;
		} else {
			WARN_PRINT(vformat("Runtime plan node %s of type %s cannot be created. A placeholder will be created instead.", plan_node->name, plan_node->type).ascii().get_data());
			if (p_parent) {
				if (Object::cast_to<Control>(p_parent)) {
					node = memnew(Control);
				} else if (Object::cast_to<Node2D>(p_parent)) {
					node = memnew(Node2D);
#ifndef _3D_DISABLED
				} else if (Object::cast_to<Node3D>(p_parent)) {
					node = memnew(Node3D);
#endif // _3D_DISABLED
				}
			}

			if (!node) {
				node = memnew(Node);
			}
		}
	}

	Node *root = p_root ? p_root : node;
	Node *owner_source_root = p_source_root ? p_source_root : node;
	Node *source_root = owner_source_root;
	if (plan_node->source_node_idx == 0) {
		source_root = node;
	}

	if (plan_node->properties.has(CoreStringName(script))) {
		bool valid = true;
		node->set(CoreStringName(script), plan_node->properties[CoreStringName(script)], &valid);
	}

	for (const KeyValue<StringName, Variant> &kv : plan_node->properties) {
		if (kv.key == CoreStringName(script)) {
			continue;
		}
		if (plan_node->deferred_node_properties.has(kv.key)) {
			if (!Engine::get_singleton()->is_editor_hint() && node->get_scene_instance_load_placeholder()) {
				bool valid = true;
				node->set(kv.key, kv.value, &valid);
			} else if (p_deferred_node_paths) {
				DeferredNodePathProperties dnp;
				dnp.value = kv.value;
				dnp.base = node->get_instance_id();
				dnp.property = kv.key;
				p_deferred_node_paths->push_back(dnp);
			}
			continue;
		}
		Variant value = kv.value;
		if (p_resources_local_to_scenes) {
			if (value.get_type() == Variant::OBJECT) {
				Ref<Resource> resource = value;
				if (resource.is_valid()) {
					value = _make_runtime_plan_local_resource(value, p_runtime_plan, p_plan_id, *p_resources_local_to_scenes, node, kv.key, root, source_root, p_edit_state);
				}
			}

			if (value.get_type() == Variant::ARRAY) {
				Array set_array = value;
				bool is_get_valid = false;
				Variant get_value = node->get(kv.key, &is_get_valid);

				if (is_get_valid && get_value.get_type() == Variant::ARRAY) {
					Array get_array = get_value;
					if (set_array.is_same_typed(get_array)) {
						set_array = set_array.duplicate();
					} else {
						set_array = Array(set_array, get_array.get_typed_builtin(), get_array.get_typed_class_name(), get_array.get_typed_script());
					}
				}

				value = _setup_runtime_plan_resources_in_array(set_array, p_runtime_plan, p_plan_id, *p_resources_local_to_scenes, node, kv.key, root, source_root, p_edit_state);
			}

			if (value.get_type() == Variant::DICTIONARY) {
				Dictionary set_dict = value;
				bool is_get_valid = false;
				Variant get_value = node->get(kv.key, &is_get_valid);

				if (is_get_valid && get_value.get_type() == Variant::DICTIONARY) {
					Dictionary get_dict = get_value;
					if (set_dict.is_same_typed(get_dict)) {
						set_dict = set_dict.duplicate();
					} else {
						set_dict = Dictionary(set_dict, get_dict.get_typed_key_builtin(), get_dict.get_typed_key_class_name(), get_dict.get_typed_key_script(), get_dict.get_typed_value_builtin(), get_dict.get_typed_value_class_name(), get_dict.get_typed_value_script());
					}
				}

				value = _setup_runtime_plan_resources_in_dictionary(set_dict, p_runtime_plan, p_plan_id, *p_resources_local_to_scenes, node, kv.key, root, source_root, p_edit_state);
			}
		}
		bool valid = true;
		node->set(kv.key, value, &valid);
	}

	for (int group_idx = 0; group_idx < source_node.groups.size(); group_idx++) {
		const int group_name_idx = source_node.groups[group_idx];
		ERR_FAIL_INDEX_V(group_name_idx, source_state->names.size(), nullptr);
		node->add_to_group(source_state->names[group_name_idx], true);
	}

	node->_set_name_nocheck(plan_node->name);

	if (p_apply_customization) {
		if (node->has_method("_customize_scene_instantiation")) {
			node->call("_customize_scene_instantiation", p_runtime_plan->_make_node_ref(p_plan_id));
		} else if (node->has_method("_filter_scene_children")) {
			_apply_legacy_filter_to_runtime_plan(node, p_runtime_plan, p_plan_id);
		}
	}

	plan_node = p_runtime_plan->_get_node_data(p_plan_id);
	ERR_FAIL_NULL_V(plan_node, nullptr);
	if (plan_node->pruned || plan_node->flattened) {
		if (p_resources_local_to_scenes && p_resources_local_to_scenes->has(node)) {
			p_resources_local_to_scenes->erase(node);
		}
		memdelete(node);
		return nullptr;
	}

	if (p_materialized_plan_nodes) {
		p_materialized_plan_nodes->insert(p_plan_id, node->get_instance_id());
	}
	if (p_parent) {
		p_parent->_add_child_nocheck(node, plan_node->name);
	}

	const NodePath owner_path = plan_node->owner_path;
	if (p_parent != nullptr && !owner_path.is_empty()) {
		Node *owner = nullptr;
		if (owner_path == NodePath(".")) {
			owner = owner_source_root;
		} else if (owner_source_root) {
			owner = owner_source_root->get_node_or_null(owner_path);
		}
		if (owner) {
			node->_set_owner_nocheck(owner);
		}
	}

	int child_idx = 0;
	while (true) {
		plan_node = p_runtime_plan->_get_node_data(p_plan_id);
		ERR_FAIL_NULL_V(plan_node, node);
		if (child_idx >= plan_node->child_plan_ids.size()) {
			break;
		}

		const int child_plan_id = plan_node->child_plan_ids[child_idx];
		Node *materialized_child = _materialize_runtime_plan_node(p_runtime_plan, child_plan_id, node, root, source_root, p_deferred_node_paths, p_resources_local_to_scenes, p_materialized_plan_nodes, p_edit_state, p_apply_customization);
		const SceneInstantiationPlan::PlanNodeData *child_plan = p_runtime_plan->has_node(child_plan_id) ? p_runtime_plan->_get_node_data(child_plan_id) : nullptr;
		if (!materialized_child && child_plan && (child_plan->pruned || child_plan->flattened)) {
			continue;
		}
		child_idx++;
	}

	return node;
}

Node *SceneState::instantiate(GenEditState p_edit_state) const {
	if (Engine::get_singleton()->is_editor_hint()) {
		return _instantiate_legacy(p_edit_state);
	}

	return _instantiate_runtime_plan(p_edit_state);
}

Variant SceneState::make_local_resource(Variant &p_value, const SceneState::NodeData &p_node_data, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_sname, int p_i, Node **p_ret_nodes, SceneState::GenEditState p_edit_state) const {
	Ref<Resource> res = p_value;
	if (res.is_null() || !res->is_local_to_scene()) {
		return p_value;
	}

	Node *base = (p_i == 0 || p_node->is_instance()) ? p_node : (p_node->get_owner() ? p_node->get_owner() : p_ret_nodes[0]);

	if (p_node_data.type == TYPE_INSTANTIATED) { // For the (root) nodes of sub-scenes, treat them as parts of the sub-scenes.
		return get_remap_resource(res, p_resources_local_to_scenes, p_node->get(p_sname), base);
	}

	// Find the shared copy of the source resource.
	HashMap<Ref<Resource>, Ref<Resource>>::Iterator R = p_resources_local_to_scenes[base].find(res);
	if (R) {
		return R->value;
	}

	if (p_edit_state == GEN_EDIT_STATE_MAIN) { // For the main scene, use the resource as is
		res->configure_for_local_scene(base, p_resources_local_to_scenes[base]);
		p_resources_local_to_scenes[base][res] = res;
		return res;
	}

	// For instances, a copy must be made.
	Ref<Resource> local_dupe = res->duplicate_for_local_scene(base, p_resources_local_to_scenes[base]);
	p_resources_local_to_scenes[base][res] = local_dupe;
	return local_dupe;
}

Array SceneState::setup_resources_in_array(Array &p_array_to_scan, const SceneState::NodeData &p_n, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_sname, int p_i, Node **p_ret_nodes, SceneState::GenEditState p_edit_state) const {
	for (int i = 0; i < p_array_to_scan.size(); i++) {
		if (p_array_to_scan[i].get_type() == Variant::OBJECT) {
			p_array_to_scan[i] = make_local_resource(p_array_to_scan[i], p_n, p_resources_local_to_scenes, p_node, p_sname, p_i, p_ret_nodes, p_edit_state);
		}
	}
	return p_array_to_scan;
}

Dictionary SceneState::setup_resources_in_dictionary(Dictionary &p_dictionary_to_scan, const SceneState::NodeData &p_n, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_sname, int p_i, Node **p_ret_nodes, SceneState::GenEditState p_edit_state) const {
	const Array keys = p_dictionary_to_scan.keys();
	const Array values = p_dictionary_to_scan.values();

	if (has_local_resource(values) || has_local_resource(keys)) {
		Array duplicated_keys = keys.duplicate(true);
		Array duplicated_values = values.duplicate(true);

		duplicated_keys = setup_resources_in_array(duplicated_keys, p_n, p_resources_local_to_scenes, p_node, p_sname, p_i, p_ret_nodes, p_edit_state);
		duplicated_values = setup_resources_in_array(duplicated_values, p_n, p_resources_local_to_scenes, p_node, p_sname, p_i, p_ret_nodes, p_edit_state);
		p_dictionary_to_scan.clear();

		for (int i = 0; i < keys.size(); i++) {
			p_dictionary_to_scan[duplicated_keys[i]] = duplicated_values[i];
		}
	}

	return p_dictionary_to_scan;
}

bool SceneState::has_local_resource(const Array &p_array) const {
	for (int i = 0; i < p_array.size(); i++) {
		Ref<Resource> res = p_array[i];
		if (res.is_valid() && res->is_local_to_scene()) {
			return true;
		}
	}
	return false;
}

static int _nm_get_string(const String &p_string, HashMap<StringName, int> &name_map) {
	if (name_map.has(p_string)) {
		return name_map[p_string];
	}

	int idx = name_map.size();
	name_map[p_string] = idx;
	return idx;
}

static int _vm_get_variant(const Variant &p_variant, HashMap<Variant, int> &variant_map) {
	if (variant_map.has(p_variant)) {
		return variant_map[p_variant];
	}

	int idx = variant_map.size();
	variant_map[p_variant] = idx;
	return idx;
}

Error SceneState::_parse_node(Node *p_owner, Node *p_node, int p_parent_idx, HashMap<StringName, int> &name_map, HashMap<Variant, int> &variant_map, HashMap<Node *, int> &node_map, HashMap<Node *, int> &nodepath_map, HashSet<int32_t> &ids_saved) {
	// this function handles all the work related to properly packing scenes, be it
	// instantiated or inherited.
	// given the complexity of this process, an attempt will be made to properly
	// document it. if you fail to understand something, please ask!

	//discard nodes that do not belong to be processed
	if (_should_skip_foreign_scene_subtree(p_owner, p_node)) {
		return OK;
	}

	bool is_editable_instance = false;

	// save the child instantiated scenes that are chosen as editable, so they can be restored
	// upon load back
	if (p_node != p_owner && p_node->is_instance() && p_owner->is_editable_instance(p_node)) {
		editable_instances.push_back(p_owner->get_path_to(p_node));
		// Node is the root of an editable instance.
		is_editable_instance = true;
	} else if (p_node->get_owner() && p_owner->is_ancestor_of(p_node->get_owner()) && p_owner->is_editable_instance(p_node->get_owner())) {
		// Node is part of an editable instance.
		is_editable_instance = true;
	}

	// Save exposed children paths.
	if (p_node != p_owner && p_owner->is_exposed_node_to_owner(p_node)) {
		exposed_children.push_back(p_owner->get_path_to(p_node));
	}

	NodeData nd;

	nd.name = _nm_get_string(p_node->get_name(), name_map);
	nd.instance = -1; //not instantiated by default

	//really convoluted condition, but it basically checks that index is only saved when part of an inherited scene OR the node parent is from the edited scene
	if (p_owner->get_scene_inherited_state().is_null() && (p_node == p_owner || (p_node->get_owner() == p_owner && (p_node->get_parent() == p_owner || p_node->get_parent()->get_owner() == p_owner)))) {
		//do not save index, because it belongs to saved scene and scene is not inherited
		nd.index = -1;
	} else if (p_node == p_owner) {
		//This (hopefully) happens if the node is a scene root, so its index is irrelevant.
		nd.index = -1;
	} else {
		//part of an inherited scene, or parent is from an instantiated scene
		nd.index = p_node->get_index();
	}

	// if this node is part of an instantiated scene or sub-instantiated scene
	// we need to get the corresponding instance states.
	// with the instance states, we can query for identical properties/groups
	// and only save what has changed

	bool instantiated_by_owner = false;
	Vector<SceneState::PackState> states_stack = PropertyUtils::get_node_states_stack(p_node, p_owner, &instantiated_by_owner);

	if (p_node->is_instance() && p_node->get_owner() == p_owner && instantiated_by_owner) {
		if (p_node->get_scene_instance_load_placeholder()) {
			//it's a placeholder, use the placeholder path
			nd.instance = _vm_get_variant(p_node->get_scene_file_path(), variant_map);
			nd.instance |= FLAG_INSTANCE_IS_PLACEHOLDER;
		} else {
			//must instance ourselves
			Ref<PackedScene> instance = ResourceLoader::load(p_node->get_scene_file_path());
			if (instance.is_null()) {
				return ERR_CANT_OPEN;
			}

			nd.instance = _vm_get_variant(instance, variant_map);
		}
	}

	// all setup, we then proceed to check all properties for the node
	// and save the ones that are worth saving

	List<PropertyInfo> plist;
	p_node->get_property_list(&plist);

	Array pinned_props = _sanitize_node_pinned_properties(p_node);
	Dictionary missing_resource_properties = p_node->get_meta(META_MISSING_RESOURCES, Dictionary());

	for (const PropertyInfo &E : plist) {
		if (!(E.usage & PROPERTY_USAGE_STORAGE) && !missing_resource_properties.has(E.name)) {
			continue;
		}

		if (E.name == META_PROPERTY_MISSING_RESOURCES) {
			continue; // Ignore this property when packing.
		}

		// If instance or inheriting, not saving if property requested so.
		if (!states_stack.is_empty()) {
			if ((E.usage & PROPERTY_USAGE_NO_INSTANCE_STATE)) {
				continue;
			}
		}

		StringName name = E.name;
		Variant value = _get_storable_property_base_value(p_node, name);
		bool use_deferred_node_path_bit = false;

		if (E.type == Variant::OBJECT && E.hint == PROPERTY_HINT_NODE_TYPE) {
			if (value.get_type() == Variant::OBJECT) {
				if (Node *n = Object::cast_to<Node>(value)) {
					value = p_node->get_path_to(n);
				}
				use_deferred_node_path_bit = true;
			}
			if (value.get_type() != Variant::NODE_PATH) {
				continue; //was never set, ignore.
			}
		} else if (E.type == Variant::OBJECT && missing_resource_properties.has(E.name)) {
			// Was this missing resource overridden? If so do not save the old value.
			Ref<Resource> ures = value;
			if (ures.is_null()) {
				value = missing_resource_properties[E.name];
			}
		} else if (E.type == Variant::ARRAY && E.hint == PROPERTY_HINT_TYPE_STRING) {
			int hint_subtype_separator = E.hint_string.find_char(':');
			if (hint_subtype_separator >= 0) {
				String subtype_string = E.hint_string.substr(0, hint_subtype_separator);
				int slash_pos = subtype_string.find_char('/');
				PropertyHint subtype_hint = PropertyHint::PROPERTY_HINT_NONE;
				if (slash_pos >= 0) {
					subtype_hint = PropertyHint(subtype_string.get_slicec('/', 1).to_int());
					subtype_string = subtype_string.substr(0, slash_pos);
				}
				Variant::Type subtype = Variant::Type(subtype_string.to_int());

				if (subtype == Variant::OBJECT && subtype_hint == PROPERTY_HINT_NODE_TYPE) {
					use_deferred_node_path_bit = true;
					Array array = value;
					Array new_array;
					for (int i = 0; i < array.size(); i++) {
						Variant elem = array[i];
						if (elem.get_type() == Variant::OBJECT) {
							if (Node *n = Object::cast_to<Node>(elem)) {
								new_array.push_back(p_node->get_path_to(n));
								continue;
							}
						}
						new_array.push_back(elem);
					}
					value = new_array;
				}
			}
		} else if (E.type == Variant::DICTIONARY && E.hint == PROPERTY_HINT_TYPE_STRING) {
			int key_value_separator = E.hint_string.find_char(';');
			if (key_value_separator >= 0) {
				int key_subtype_separator = E.hint_string.find_char(':');
				String key_subtype_string = E.hint_string.substr(0, key_subtype_separator);
				int key_slash_pos = key_subtype_string.find_char('/');
				PropertyHint key_subtype_hint = PropertyHint::PROPERTY_HINT_NONE;
				if (key_slash_pos >= 0) {
					key_subtype_hint = PropertyHint(key_subtype_string.get_slicec('/', 1).to_int());
					key_subtype_string = key_subtype_string.substr(0, key_slash_pos);
				}
				Variant::Type key_subtype = Variant::Type(key_subtype_string.to_int());
				bool convert_key = key_subtype == Variant::OBJECT && key_subtype_hint == PROPERTY_HINT_NODE_TYPE;

				int value_subtype_separator = E.hint_string.find_char(':', key_value_separator) - (key_value_separator + 1);
				String value_subtype_string = E.hint_string.substr(key_value_separator + 1, value_subtype_separator);
				int value_slash_pos = value_subtype_string.find_char('/');
				PropertyHint value_subtype_hint = PropertyHint::PROPERTY_HINT_NONE;
				if (value_slash_pos >= 0) {
					value_subtype_hint = PropertyHint(value_subtype_string.get_slicec('/', 1).to_int());
					value_subtype_string = value_subtype_string.substr(0, value_slash_pos);
				}
				Variant::Type value_subtype = Variant::Type(value_subtype_string.to_int());
				bool convert_value = value_subtype == Variant::OBJECT && value_subtype_hint == PROPERTY_HINT_NODE_TYPE;

				if (convert_key || convert_value) {
					use_deferred_node_path_bit = true;
					Dictionary dict = value;
					Dictionary new_dict;
					for (const KeyValue<Variant, Variant> &kv : dict) {
						Variant new_key = kv.key;
						if (convert_key && new_key.get_type() == Variant::OBJECT) {
							if (Node *n = Object::cast_to<Node>(new_key)) {
								new_key = p_node->get_path_to(n);
							}
						}
						Variant new_value = kv.value;
						if (convert_value && new_value.get_type() == Variant::OBJECT) {
							if (Node *n = Object::cast_to<Node>(new_value)) {
								new_value = p_node->get_path_to(n);
							}
						}
						new_dict[new_key] = new_value;
					}
					value = new_dict;
				}
			}
		}

		if (!pinned_props.has(name)) {
			bool is_valid_default = false;
			Variant default_value = PropertyUtils::get_property_default_value(p_node, name, &is_valid_default, &states_stack, true);

			if (is_valid_default && !PropertyUtils::is_property_value_different(p_node, value, default_value)) {
				if (value.get_type() == Variant::ARRAY && has_local_resource(value)) {
					// Save anyway
				} else if (value.get_type() == Variant::DICTIONARY) {
					Dictionary dictionary = value;
					if (!has_local_resource(dictionary.values()) && !has_local_resource(dictionary.keys())) {
						continue;
					}
				} else {
					continue;
				}
			}
		}

		NodeData::Property prop;
		prop.name = _nm_get_string(name, name_map);
		prop.value = _vm_get_variant(value, variant_map);
		if (use_deferred_node_path_bit) {
			prop.name |= FLAG_PATH_PROPERTY_IS_NODE;
		}
		nd.properties.push_back(prop);
	}

	// save the groups this node is into
	// discard groups that come from the original scene

	List<Node::GroupInfo> groups;
	p_node->get_groups(&groups);
	for (const Node::GroupInfo &gi : groups) {
		if (!gi.persistent) {
			continue;
		}

		bool skip = false;
		for (const SceneState::PackState &ia : states_stack) {
			//check all levels of pack to see if the group was added somewhere
			if (ia.state->is_node_in_group(ia.node, gi.name)) {
				skip = true;
				break;
			}
		}

		if (skip) {
			continue;
		}

		nd.groups.push_back(_nm_get_string(gi.name, name_map));
	}

	// save the right owner
	// for the saved scene root this is -1
	// for nodes of the saved scene this is 0
	// for nodes of instantiated scenes this is >0

	if (p_node == p_owner) {
		//saved scene root
		nd.owner = -1;
	} else if (p_node->get_owner() == p_owner) {
		//part of saved scene
		nd.owner = 0;
	} else {
		nd.owner = -1;
	}

	MissingNode *missing_node = Object::cast_to<MissingNode>(p_node);

	// Save the right type. If this node was created by an instance
	// then flag that the node should not be created but reused
	if (states_stack.is_empty() && !is_editable_instance) {
		//This node is not part of an instantiation process, so save the type.
		if (missing_node != nullptr) {
			// It's a missing node (type non existent on load).
			nd.type = _nm_get_string(missing_node->get_original_class(), name_map);
		} else {
			nd.type = _nm_get_string(p_node->get_class(), name_map);
		}
	} else {
		// this node is part of an instantiated process, so do not save the type.
		// instead, save that it was instantiated
		nd.type = TYPE_INSTANTIATED;
	}

	// determine whether to save this node or not
	// if this node is part of an instantiated sub-scene, we can skip storing it if basically
	// no properties changed and no groups were added to it.
	// below condition is true for all nodes of the scene being saved, and ones in subscenes
	// that hold changes

	bool save_node = p_node == p_owner; // owner is always saved
	save_node = save_node || (p_node->get_owner() == p_owner && instantiated_by_owner); //part of scene and not instanced
	bool save_data = nd.properties.size() || nd.groups.size(); // some local properties or groups exist

	int idx = nodes.size();
	int parent_node = NO_PARENT_SAVED;

	if (save_node || save_data) {
		//don't save the node if nothing and subscene

		node_map[p_node] = idx;

		//ok validate parent node
		if (p_parent_idx == NO_PARENT_SAVED) {
			int sidx;
			if (nodepath_map.has(p_node->get_parent())) {
				sidx = nodepath_map[p_node->get_parent()];
			} else {
				sidx = nodepath_map.size();
				nodepath_map[p_node->get_parent()] = sidx;
			}

			nd.parent = FLAG_ID_IS_PATH | sidx;
		} else {
			nd.parent = p_parent_idx;
		}

		int32_t unique_scene_id = p_node->get_unique_scene_id();
		if (save_node && (unique_scene_id == Node::UNIQUE_SCENE_ID_UNASSIGNED || ids_saved.has(unique_scene_id))) {
			// Unassigned or clash somehow.
			// Clashes will always happen with instantiated scenes, so it is normal
			// to expect them to be resolved.

			while (true) {
				uint32_t data = ResourceUID::get_singleton()->create_id();
				unique_scene_id = data & 0x7FFFFFFF; // keep positive.

				if (unique_scene_id == Node::UNIQUE_SCENE_ID_UNASSIGNED) {
					unique_scene_id = 1;
				}
				if (ids_saved.has(unique_scene_id)) {
					// While there is one in a four billion chance for a clash, the scenario where one scene is instantiated multiple times is common, so it must reassign the local id.
					continue;
				}
				break;
			}

			p_node->set_unique_scene_id(unique_scene_id);
		}

		ids_saved.insert(unique_scene_id);
		ids.push_back(unique_scene_id);

		parent_node = idx;
		nodes.push_back(nd);
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *c = p_node->get_child(i);
		Error err = _parse_node(p_owner, c, parent_node, name_map, variant_map, node_map, nodepath_map, ids_saved);
		if (err) {
			return err;
		}
	}

	return OK;
}

Error SceneState::_parse_connections(Node *p_owner, Node *p_node, HashMap<StringName, int> &name_map, HashMap<Variant, int> &variant_map, HashMap<Node *, int> &node_map, HashMap<Node *, int> &nodepath_map) {
	// Ignore nodes that are within a scene instance.
	if (_should_skip_foreign_scene_subtree(p_owner, p_node)) {
		return OK;
	}

	List<MethodInfo> _signals;
	p_node->get_signal_list(&_signals);
	_signals.sort();

	//ERR_FAIL_COND_V( !node_map.has(p_node), ERR_BUG);
	//NodeData &nd = nodes[node_map[p_node]];

	for (const MethodInfo &E : _signals) {
		List<Node::Connection> conns;
		p_node->get_signal_connection_list(E.name, &conns);

		conns.sort();

		for (const Node::Connection &F : conns) {
			const Node::Connection &c = F;

			// Don't save connections that are not persistent.
			if (!(c.flags & CONNECT_PERSIST)) {
				continue;
			}

			// only connections that originate or end into main saved scene are saved
			// everything else is discarded

			Node *target = Object::cast_to<Node>(c.callable.get_object());

			if (!target) {
				continue;
			}

			Vector<Variant> binds;
			int unbinds = 0;
			Callable base_callable;

			if (c.callable.is_custom()) {
				CallableCustomBind *ccb = dynamic_cast<CallableCustomBind *>(c.callable.get_custom());
				if (ccb) {
					binds = ccb->get_binds();
					unbinds = ccb->get_unbound_arguments_count();

					base_callable = ccb->get_callable();
				}

				CallableCustomUnbind *ccu = dynamic_cast<CallableCustomUnbind *>(c.callable.get_custom());
				if (ccu) {
					ccu->get_bound_arguments(binds);
					unbinds = ccu->get_unbinds();
					base_callable = ccu->get_callable();
				}
			} else {
				base_callable = c.callable;
			}

			//find if this connection already exists
			Node *common_parent = target->find_common_parent_with(p_node);

			ERR_CONTINUE(!common_parent);

			if (common_parent != p_owner && !common_parent->is_instance()) {
				common_parent = common_parent->get_owner();
			}

			bool exists = false;

			//go through ownership chain to see if this exists
			while (common_parent) {
				Ref<SceneState> ps;

				if (common_parent == p_owner) {
					ps = common_parent->get_scene_inherited_state();
				} else {
					ps = common_parent->get_scene_instance_state();
				}

				if (ps.is_valid()) {
					NodePath signal_from = common_parent->get_path_to(p_node);
					NodePath signal_to = common_parent->get_path_to(target);

					if (ps->has_connection(signal_from, c.signal.get_name(), signal_to, base_callable.get_method())) {
						exists = true;
						break;
					}
				}

				if (common_parent == p_owner) {
					break;
				} else {
					common_parent = common_parent->get_owner();
				}
			}

			if (exists) { //already exists (comes from instance or inheritance), so don't save
				continue;
			}

			{
				Node *nl = p_node;

				bool exists2 = false;

				while (nl) {
					if (nl == p_owner) {
						Ref<SceneState> state = nl->get_scene_inherited_state();
						if (state.is_valid()) {
							int from_node = state->find_node_by_path(nl->get_path_to(p_node));
							int to_node = state->find_node_by_path(nl->get_path_to(target));

							if (from_node >= 0 && to_node >= 0) {
								//this one has state for this node, save
								if (state->is_connection(from_node, c.signal.get_name(), to_node, base_callable.get_method())) {
									exists2 = true;
									break;
								}
							}
						}

						nl = nullptr;
					} else {
						if (nl->is_instance()) {
							Ref<SceneState> state = nl->get_scene_instance_state();
							if (state.is_valid()) {
								int from_node = state->find_node_by_path(nl->get_path_to(p_node));
								int to_node = state->find_node_by_path(nl->get_path_to(target));

								if (from_node >= 0 && to_node >= 0) {
									//this one has state for this node, save
									if (state->is_connection(from_node, c.signal.get_name(), to_node, base_callable.get_method())) {
										exists2 = true;
										break;
									}
								}
							}
						}
						nl = nl->get_owner();
					}
				}

				if (exists2) {
					continue;
				}
			}

			int src_id;

			if (node_map.has(p_node)) {
				src_id = node_map[p_node];
			} else {
				if (nodepath_map.has(p_node)) {
					src_id = FLAG_ID_IS_PATH | nodepath_map[p_node];
				} else {
					int sidx = nodepath_map.size();
					nodepath_map[p_node] = sidx;
					src_id = FLAG_ID_IS_PATH | sidx;
				}
			}

			int target_id;

			if (node_map.has(target)) {
				target_id = node_map[target];
			} else {
				if (nodepath_map.has(target)) {
					target_id = FLAG_ID_IS_PATH | nodepath_map[target];
				} else {
					int sidx = nodepath_map.size();
					nodepath_map[target] = sidx;
					target_id = FLAG_ID_IS_PATH | sidx;
				}
			}

			ConnectionData cd;
			cd.from = src_id;
			cd.to = target_id;
			cd.method = _nm_get_string(base_callable.get_method(), name_map);
			cd.signal = _nm_get_string(c.signal.get_name(), name_map);
			cd.flags = c.flags & ~CONNECT_INHERITED; // Do not store inherited.
			cd.unbinds = unbinds;

			for (int i = 0; i < binds.size(); i++) {
				cd.binds.push_back(_vm_get_variant(binds[i], variant_map));
			}
			connections.push_back(cd);
		}
	}

	// Recursively parse child connections.
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *child = p_node->get_child(i);
		Error err = _parse_connections(p_owner, child, name_map, variant_map, node_map, nodepath_map);
		if (err) {
			return err;
		}
	}

	return OK;
}

Error SceneState::pack(Node *p_scene) {
	ERR_FAIL_NULL_V(p_scene, ERR_INVALID_PARAMETER);

	clear();

	Node *scene = p_scene;

	HashMap<StringName, int> name_map;
	HashMap<Variant, int> variant_map;
	HashMap<Node *, int> node_map;
	HashMap<Node *, int> nodepath_map;
	HashSet<int32_t> ids_saved;

	// If using scene inheritance, pack the scene it inherits from.
	if (scene->get_scene_inherited_state().is_valid()) {
		String scene_path = scene->get_scene_inherited_state()->get_path();
		Ref<PackedScene> instance = ResourceLoader::load(scene_path);
		if (instance.is_valid()) {
			base_scene_idx = _vm_get_variant(instance, variant_map);
		}
	}

	// Instanced, only direct sub-scenes are supported of course.
	Error err = _parse_node(scene, scene, -1, name_map, variant_map, node_map, nodepath_map, ids_saved);
	if (err) {
		clear();
		ERR_FAIL_V(err);
	}

	err = _parse_connections(scene, scene, name_map, variant_map, node_map, nodepath_map);
	if (err) {
		clear();
		ERR_FAIL_V(err);
	}

	names.resize(name_map.size());

	for (const KeyValue<StringName, int> &E : name_map) {
		names.write[E.value] = E.key;
	}

	variants.resize(variant_map.size());

	for (const KeyValue<Variant, int> &E : variant_map) {
		int idx = E.value;
		variants.write[idx] = E.key;
	}

	node_paths.resize(nodepath_map.size());
	id_paths.resize(nodepath_map.size());
	for (const KeyValue<Node *, int> &E : nodepath_map) {
		node_paths.write[E.value] = scene->get_path_to(E.key);

		// Build a path of IDs to reach the node.
		PackedInt32Array id_path;
		bool id_path_valid = false;
		Node *base = E.key;
		while (base && base->get_unique_scene_id() != Node::UNIQUE_SCENE_ID_UNASSIGNED) {
			id_path.push_back(base->get_unique_scene_id());
			base = base->get_owner();
			if (base == p_scene) {
				id_path_valid = true;
				break;
			}
		}

		if (!id_path_valid) {
			id_path.clear();
		}

		// Reverse it since we went from node to owner, and we seek from owner to node.
		id_path.reverse();

		id_paths.write[E.value] = id_path;
	}

	if (Engine::get_singleton()->is_editor_hint()) {
		// Build node path cache
		for (const KeyValue<Node *, int> &E : node_map) {
			node_path_cache[scene->get_path_to(E.key)] = E.value;
		}
	}

	return OK;
}

void SceneState::set_path(const String &p_path) {
	path = p_path;
}

String SceneState::get_path() const {
	return path;
}

void SceneState::clear() {
	names.clear();
	variants.clear();
	nodes.clear();
	connections.clear();
	node_path_cache.clear();
	node_paths.clear();
	editable_instances.clear();
	exposed_children.clear();
	ids.clear();
	id_paths.clear();
	base_scene_idx = -1;
}

Error SceneState::copy_from(const Ref<SceneState> &p_scene_state) {
	ERR_FAIL_COND_V(p_scene_state.is_null(), ERR_INVALID_PARAMETER);

	clear();

	for (const StringName &E : p_scene_state->names) {
		names.append(E);
	}
	for (const Variant &E : p_scene_state->variants) {
		variants.append(E);
	}
	for (const SceneState::NodeData &E : p_scene_state->nodes) {
		nodes.append(E);
	}
	for (const SceneState::ConnectionData &E : p_scene_state->connections) {
		connections.append(E);
	}
	for (KeyValue<NodePath, int> &E : p_scene_state->node_path_cache) {
		node_path_cache.insert(E.key, E.value);
	}
	for (const NodePath &E : p_scene_state->node_paths) {
		node_paths.append(E);
	}
	for (const PackedInt32Array &E : p_scene_state->id_paths) {
		id_paths.append(E);
	}
	for (const NodePath &E : p_scene_state->editable_instances) {
		editable_instances.append(E);
	}
	for (const NodePath &E : p_scene_state->exposed_children) {
		exposed_children.append(E);
	}
	base_scene_idx = p_scene_state->base_scene_idx;

	return OK;
}

Ref<SceneState> SceneState::get_base_scene_state() const {
	if (base_scene_idx >= 0) {
		Ref<PackedScene> ps = variants[base_scene_idx];
		if (ps.is_valid()) {
			return ps->get_state();
		}
	}

	return Ref<SceneState>();
}

int SceneState::find_node_by_path(const NodePath &p_node) const {
	ERR_FAIL_COND_V_MSG(node_path_cache.is_empty(), -1, "This operation requires the node cache to have been built.");

	if (!node_path_cache.has(p_node)) {
		// If not in this scene state, find node path by scene inheritance.
		if (get_base_scene_state().is_valid()) {
			int idx = get_base_scene_state()->find_node_by_path(p_node);
			if (idx != -1) {
				int rkey = _find_base_scene_node_remap_key(idx);
				if (rkey == -1) {
					rkey = nodes.size() + base_scene_node_remap.size();
					base_scene_node_remap[rkey] = idx;
				}
				return rkey;
			}
		}
		return -1;
	}

	int nid = node_path_cache[p_node];

	if (get_base_scene_state().is_valid() && !base_scene_node_remap.has(nid)) {
		//for nodes that _do_ exist in current scene, still try to look for
		//the node in the instantiated scene, as a property may be missing
		//from the local one
		int idx = get_base_scene_state()->find_node_by_path(p_node);
		if (idx != -1) {
			base_scene_node_remap[nid] = idx;
		}
	}

	return nid;
}

int SceneState::_find_base_scene_node_remap_key(int p_idx) const {
	for (const KeyValue<int, int> &E : base_scene_node_remap) {
		if (E.value == p_idx) {
			return E.key;
		}
	}
	return -1;
}

Variant SceneState::get_property_value(int p_node, const StringName &p_property, bool &r_found, bool &r_node_deferred) const {
	r_found = false;
	r_node_deferred = false;

	ERR_FAIL_COND_V(p_node < 0, Variant());

	if (p_node < nodes.size()) {
		// Find in built-in nodes.
		int pc = nodes[p_node].properties.size();
		const StringName *namep = names.ptr();

		const NodeData::Property *p = nodes[p_node].properties.ptr();
		for (int i = 0; i < pc; i++) {
			if (p_property == namep[p[i].name & FLAG_PROP_NAME_MASK]) {
				r_found = true;
				r_node_deferred = p[i].name & FLAG_PATH_PROPERTY_IS_NODE;
				return variants[p[i].value];
			}
		}

#ifndef DISABLE_DEPRECATED
#ifdef TOOLS_ENABLED
		// Compatibility: In 4.5 and earlier, AnimationMixer used a single "libraries" Dictionary property.
		// In 4.6+, each library is stored as a separate "libraries/<name>" property.
		// If we're looking for "libraries/<name>" and didn't find it, check the old format.
		String prop_str = p_property.string();
		if (prop_str.begins_with("libraries/")) {
			StringName node_type = get_node_type(p_node);
			if (node_type != StringName() && ClassDB::is_parent_class(node_type, SNAME("AnimationMixer"))) {
				String library_name = prop_str.get_slicec('/', 1);
				static const StringName libraries_sname = "libraries";
				for (int i = 0; i < pc; i++) {
					if (namep[p[i].name & FLAG_PROP_NAME_MASK] == libraries_sname) {
						Variant libs_variant = variants[p[i].value];
						if (libs_variant.get_type() == Variant::DICTIONARY) {
							Dictionary libs_dict = libs_variant;
							if (libs_dict.has(library_name)) {
								r_found = true;
								r_node_deferred = false;
								return libs_dict[library_name];
							}
						}
						break;
					}
				}
			}
		}
#endif // TOOLS_ENABLED
#endif // DISABLE_DEPRECATED
	}

	// Property not found, try on instance.
	HashMap<int, int>::ConstIterator I = base_scene_node_remap.find(p_node);
	if (I) {
		return get_base_scene_state()->get_property_value(I->value, p_property, r_found, r_node_deferred);
	}

	return Variant();
}

bool SceneState::is_node_in_group(int p_node, const StringName &p_group) const {
	ERR_FAIL_COND_V(p_node < 0, false);

	if (p_node < nodes.size()) {
		const StringName *namep = names.ptr();
		for (int i = 0; i < nodes[p_node].groups.size(); i++) {
			if (namep[nodes[p_node].groups[i]] == p_group) {
				return true;
			}
		}
	}

	if (base_scene_node_remap.has(p_node)) {
		return get_base_scene_state()->is_node_in_group(base_scene_node_remap[p_node], p_group);
	}

	return false;
}

bool SceneState::disable_placeholders = false;

void SceneState::set_disable_placeholders(bool p_disable) {
	disable_placeholders = p_disable;
}

bool SceneState::is_connection(int p_node, const StringName &p_signal, int p_to_node, const StringName &p_to_method) const {
	ERR_FAIL_COND_V(p_node < 0, false);
	ERR_FAIL_COND_V(p_to_node < 0, false);

	if (p_node < nodes.size() && p_to_node < nodes.size()) {
		int signal_idx = -1;
		int method_idx = -1;
		for (int i = 0; i < names.size(); i++) {
			if (names[i] == p_signal) {
				signal_idx = i;
			} else if (names[i] == p_to_method) {
				method_idx = i;
			}
		}

		if (signal_idx >= 0 && method_idx >= 0) {
			//signal and method strings are stored..

			for (int i = 0; i < connections.size(); i++) {
				if (connections[i].from == p_node && connections[i].to == p_to_node && connections[i].signal == signal_idx && connections[i].method == method_idx) {
					return true;
				}
			}
		}
	}

	if (base_scene_node_remap.has(p_node) && base_scene_node_remap.has(p_to_node)) {
		return get_base_scene_state()->is_connection(base_scene_node_remap[p_node], p_signal, base_scene_node_remap[p_to_node], p_to_method);
	}

	return false;
}

void SceneState::set_bundled_scene(const Dictionary &p_dictionary) {
	ERR_FAIL_COND(!p_dictionary.has("names"));
	ERR_FAIL_COND(!p_dictionary.has("variants"));
	ERR_FAIL_COND(!p_dictionary.has("node_count"));
	ERR_FAIL_COND(!p_dictionary.has("nodes"));
	ERR_FAIL_COND(!p_dictionary.has("conn_count"));
	ERR_FAIL_COND(!p_dictionary.has("conns"));
	//ERR_FAIL_COND( !p_dictionary.has("path"));

	int version = 1;
	if (p_dictionary.has("version")) {
		version = p_dictionary["version"];
	}

	ERR_FAIL_COND_MSG(version > PACKED_SCENE_VERSION, "Save format version too new.");

	const int node_count = p_dictionary["node_count"];
	const Vector<int> snodes = p_dictionary["nodes"];
	ERR_FAIL_COND(snodes.size() < node_count);

	const int conn_count = p_dictionary["conn_count"];
	const Vector<int> sconns = p_dictionary["conns"];
	ERR_FAIL_COND(sconns.size() < conn_count);

	Vector<String> snames = p_dictionary["names"];
	if (snames.size()) {
		int namecount = snames.size();
		names.resize(namecount);
		const String *r = snames.ptr();
		for (int i = 0; i < names.size(); i++) {
			names.write[i] = r[i];
		}
	}

	Array svariants = p_dictionary["variants"];

	if (svariants.size()) {
		int varcount = svariants.size();
		variants.resize(varcount);
		for (int i = 0; i < varcount; i++) {
			variants.write[i] = svariants[i];
		}

	} else {
		variants.clear();
	}

	nodes.resize(node_count);
	if (node_count) {
		const int *r = snodes.ptr();
		int idx = 0;
		for (int i = 0; i < node_count; i++) {
			NodeData &nd = nodes.write[i];
			nd.parent = r[idx++];
			nd.owner = r[idx++];
			nd.type = r[idx++];
			uint32_t name_index = r[idx++];
			nd.name = name_index & ((1 << NAME_INDEX_BITS) - 1);
			nd.index = (name_index >> NAME_INDEX_BITS);
			nd.index--; //0 is invalid, stored as 1
			nd.instance = r[idx++];
			nd.properties.resize(r[idx++]);
			for (int j = 0; j < nd.properties.size(); j++) {
				nd.properties.write[j].name = r[idx++];
				nd.properties.write[j].value = r[idx++];
			}
			nd.groups.resize(r[idx++]);
			for (int j = 0; j < nd.groups.size(); j++) {
				nd.groups.write[j] = r[idx++];
			}
		}
	}

	connections.resize(conn_count);
	if (conn_count) {
		const int *r = sconns.ptr();
		int idx = 0;
		for (int i = 0; i < conn_count; i++) {
			ConnectionData &cd = connections.write[i];
			cd.from = r[idx++];
			cd.to = r[idx++];
			cd.signal = r[idx++];
			cd.method = r[idx++];
			cd.flags = r[idx++];
			cd.binds.resize(r[idx++]);

			for (int j = 0; j < cd.binds.size(); j++) {
				cd.binds.write[j] = r[idx++];
			}
			if (version >= 3) {
				cd.unbinds = r[idx++];
			}
		}
	}

	if (p_dictionary.has("node_ids")) {
		ids = p_dictionary["node_ids"];
	}

	Array np;
	if (p_dictionary.has("node_paths")) {
		np = p_dictionary["node_paths"];
	}

	node_paths.resize(np.size());
	for (int i = 0; i < np.size(); i++) {
		node_paths.write[i] = np[i];
	}

	Array idp;
	if (p_dictionary.has("id_paths") && ids.size()) {
		idp = p_dictionary["id_paths"];
	}

	id_paths.resize(idp.size());
	for (int i = 0; i < idp.size(); i++) {
		id_paths.write[i] = idp[i];
	}

	Array ei;
	if (p_dictionary.has("editable_instances")) {
		ei = p_dictionary["editable_instances"];
	}

	if (p_dictionary.has("base_scene")) {
		base_scene_idx = p_dictionary["base_scene"];
	}

	editable_instances.resize(ei.size());
	for (int i = 0; i < editable_instances.size(); i++) {
		editable_instances.write[i] = ei[i];
	}

	Array ec;
	if (p_dictionary.has("exposed_children")) {
		ec = p_dictionary["exposed_children"];
	}

	exposed_children.resize(ec.size());
	for (int i = 0; i < exposed_children.size(); i++) {
		exposed_children.write[i] = ec[i];
	}

	//path=p_dictionary["path"];
}

Dictionary SceneState::get_bundled_scene() const {
	Vector<String> rnames;
	rnames.resize(names.size());

	if (names.size()) {
		String *r = rnames.ptrw();

		for (int i = 0; i < names.size(); i++) {
			r[i] = names[i];
		}
	}

	Dictionary d;
	d["names"] = rnames;
	d["variants"] = variants;

	Vector<int> rnodes;
	d["node_count"] = nodes.size();

	for (int i = 0; i < nodes.size(); i++) {
		const NodeData &nd = nodes[i];
		rnodes.push_back(nd.parent);
		rnodes.push_back(nd.owner);
		rnodes.push_back(nd.type);
		uint32_t name_index = nd.name;
		if (nd.index < (1 << (32 - NAME_INDEX_BITS)) - 1) { //save if less than 16k children
			name_index |= uint32_t(nd.index + 1) << NAME_INDEX_BITS; //for backwards compatibility, index 0 is no index
		}
		rnodes.push_back(name_index);
		rnodes.push_back(nd.instance);
		rnodes.push_back(nd.properties.size());
		for (int j = 0; j < nd.properties.size(); j++) {
			rnodes.push_back(nd.properties[j].name);
			rnodes.push_back(nd.properties[j].value);
		}
		rnodes.push_back(nd.groups.size());
		for (int j = 0; j < nd.groups.size(); j++) {
			rnodes.push_back(nd.groups[j]);
		}
	}

	d["nodes"] = rnodes;
	d["node_ids"] = ids;

	Vector<int> rconns;
	d["conn_count"] = connections.size();

	for (int i = 0; i < connections.size(); i++) {
		const ConnectionData &cd = connections[i];
		rconns.push_back(cd.from);
		rconns.push_back(cd.to);
		rconns.push_back(cd.signal);
		rconns.push_back(cd.method);
		rconns.push_back(cd.flags);
		rconns.push_back(cd.binds.size());
		for (int j = 0; j < cd.binds.size(); j++) {
			rconns.push_back(cd.binds[j]);
		}
		rconns.push_back(cd.unbinds);
	}

	d["conns"] = rconns;

	Array rnode_paths;
	rnode_paths.resize(node_paths.size());
	for (int i = 0; i < node_paths.size(); i++) {
		rnode_paths[i] = node_paths[i];
	}
	d["node_paths"] = rnode_paths;

	Array rid_paths;
	rid_paths.resize(id_paths.size());
	for (int i = 0; i < id_paths.size(); i++) {
		rid_paths[i] = id_paths[i];
	}
	d["id_paths"] = rid_paths;

	Array reditable_instances;
	reditable_instances.resize(editable_instances.size());
	for (int i = 0; i < editable_instances.size(); i++) {
		reditable_instances[i] = editable_instances[i];
	}
	d["editable_instances"] = reditable_instances;

	Array rexposed_children;
	rexposed_children.resize(exposed_children.size());
	for (int i = 0; i < exposed_children.size(); i++) {
		rexposed_children[i] = exposed_children[i];
	}
	d["exposed_children"] = rexposed_children;

	if (base_scene_idx >= 0) {
		d["base_scene"] = base_scene_idx;
	}

	d["version"] = PACKED_SCENE_VERSION;

	return d;
}

int SceneState::get_node_count() const {
	return nodes.size();
}

StringName SceneState::get_node_type(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), StringName());
	if (nodes[p_idx].type == TYPE_INSTANTIATED) {
		return StringName();
	}
	return names[nodes[p_idx].type];
}

StringName SceneState::get_node_name(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), StringName());
	return names[nodes[p_idx].name];
}

int SceneState::get_node_index(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), -1);
	return nodes[p_idx].index;
}

bool SceneState::is_node_instance_placeholder(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), false);

	return nodes[p_idx].instance >= 0 && (nodes[p_idx].instance & FLAG_INSTANCE_IS_PLACEHOLDER);
}

Ref<PackedScene> SceneState::get_node_instance(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), Ref<PackedScene>());

	if (nodes[p_idx].instance >= 0) {
		if (nodes[p_idx].instance & FLAG_INSTANCE_IS_PLACEHOLDER) {
			return Ref<PackedScene>();
		} else {
			return variants[nodes[p_idx].instance & FLAG_MASK];
		}
	} else if (nodes[p_idx].parent < 0 || nodes[p_idx].parent == NO_PARENT_SAVED) {
		if (base_scene_idx >= 0) {
			return variants[base_scene_idx];
		}
	}

	return Ref<PackedScene>();
}

String SceneState::get_node_instance_placeholder(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), String());

	if (nodes[p_idx].instance >= 0 && (nodes[p_idx].instance & FLAG_INSTANCE_IS_PLACEHOLDER)) {
		return variants[nodes[p_idx].instance & FLAG_MASK];
	}

	return String();
}

Vector<StringName> SceneState::get_node_groups(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), Vector<StringName>());
	Vector<StringName> groups;
	for (int i = 0; i < nodes[p_idx].groups.size(); i++) {
		groups.push_back(names[nodes[p_idx].groups[i]]);
	}
	return groups;
}

Node *SceneState::_recover_node_path_index(Node *p_base, int p_idx) const {
	// ID paths are only used for recovery, since they are slower to traverse.
	// This function attempts to recover a node by using IDs in case the path
	// has disappeared.
	if (p_idx >= id_paths.size()) {
		return nullptr;
	}

	const PackedInt32Array &id_path = id_paths[p_idx & FLAG_MASK];

	Vector<StringName> full_path;
	const SceneState *ss = this;
	for (int i = 0; i < id_path.size(); i++) {
		int idx = ss->ids.find(id_path[i]);
		if (idx == -1) {
			// Not found, but may belong to a base scene, so search.
			while (ss && idx == -1 && ss->base_scene_idx >= 0) {
				Ref<PackedScene> sdata = ss->variants[ss->base_scene_idx];
				if (sdata.is_null()) {
					return nullptr;
				}
				Ref<SceneState> ssd = sdata->get_state();
				if (!ssd.is_valid()) {
					return nullptr;
				}
				ss = ssd.ptr();
				idx = ss->ids.find(id_path[i]);
				if (idx != -1) {
					break;
				}
			}
			if (idx == -1) {
				//No luck.
				return nullptr;
			}
		}
		ERR_FAIL_COND_V(idx >= ss->nodes.size(), nullptr); // Should be a node.

		NodePath so_far = ss->get_node_path(idx);
		for (int j = 0; j < so_far.get_name_count(); j++) {
			full_path.push_back(so_far.get_name(j));
		}

		if (i == id_path.size() - 1) {
			break; // Do not go further, we have the path.
		}

		const NodeData &nd = ss->nodes[idx];
		// Get instance
		ERR_FAIL_COND_V(nd.instance < 0, nullptr); // Not an instance, middle of path should be an instance.
		ERR_FAIL_COND_V(nd.instance & FLAG_INSTANCE_IS_PLACEHOLDER, nullptr); // Instance is somehow a placeholder?!
		Ref<PackedScene> sdata = ss->variants[nd.instance & FLAG_MASK];
		ERR_FAIL_COND_V(sdata.is_null(), nullptr);
		Ref<SceneState> sstate = sdata->get_state();
		ss = sstate.ptr();
	}

	NodePath recovered_path(full_path, false);
	return p_base->get_node_or_null(recovered_path);
}

int32_t SceneState::get_node_unique_id(int p_idx) const {
	if (p_idx >= ids.size()) {
		return Node::UNIQUE_SCENE_ID_UNASSIGNED;
	}
	return ids[p_idx];
}

NodePath SceneState::get_node_path(int p_idx, bool p_for_parent) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), NodePath());

	if (nodes[p_idx].parent < 0 || nodes[p_idx].parent == NO_PARENT_SAVED) {
		if (p_for_parent) {
			return NodePath();
		} else {
			return NodePath(".");
		}
	}

	Vector<StringName> sub_path;
	NodePath base_path;
	int nidx = p_idx;
	while (true) {
		if (nodes[nidx].parent == NO_PARENT_SAVED || nodes[nidx].parent < 0) {
			sub_path.insert(0, ".");
			break;
		}

		if (!p_for_parent || p_idx != nidx) {
			sub_path.insert(0, names[nodes[nidx].name]);
		}

		if (nodes[nidx].parent & FLAG_ID_IS_PATH) {
			base_path = node_paths[nodes[nidx].parent & FLAG_MASK];
			break;
		} else {
			nidx = nodes[nidx].parent & FLAG_MASK;
		}
	}

	for (int i = base_path.get_name_count() - 1; i >= 0; i--) {
		sub_path.insert(0, base_path.get_name(i));
	}

	if (sub_path.is_empty()) {
		return NodePath(".");
	}

	return NodePath(sub_path, false);
}

PackedInt32Array SceneState::get_node_id_path(int p_idx) const {
	PackedInt32Array pp = get_node_parent_id_path(p_idx);
	if (pp.is_empty()) {
		return pp;
	}

	if (p_idx < ids.size()) {
		pp.push_back(ids[p_idx]);
		return pp;
	}

	return PackedInt32Array();
}

PackedInt32Array SceneState::get_node_parent_id_path(int p_idx) const {
	if (nodes[p_idx].parent < 0 || nodes[p_idx].parent == NO_PARENT_SAVED) {
		return PackedInt32Array();
	}

	if (nodes[p_idx].parent & FLAG_ID_IS_PATH) {
		int id = nodes[p_idx].parent & FLAG_MASK;
		if (id >= id_paths.size()) {
			return PackedInt32Array();
		}
		return id_paths[id];
	}

	return PackedInt32Array();
}

PackedInt32Array SceneState::get_node_owner_id_path(int p_idx) const {
	if (nodes[p_idx].owner < 0) {
		return PackedInt32Array();
	}

	if (nodes[p_idx].owner & FLAG_ID_IS_PATH) {
		int id = nodes[p_idx].owner & FLAG_MASK;
		if (id >= id_paths.size()) {
			return PackedInt32Array();
		}
		return id_paths[id];
	}

	return PackedInt32Array();
}

int SceneState::get_node_property_count(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), -1);
	return nodes[p_idx].properties.size();
}

StringName SceneState::get_node_property_name(int p_idx, int p_prop) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), StringName());
	ERR_FAIL_INDEX_V(p_prop, nodes[p_idx].properties.size(), StringName());
	return names[nodes[p_idx].properties[p_prop].name & FLAG_PROP_NAME_MASK];
}

Vector<String> SceneState::get_node_deferred_nodepath_properties(int p_idx) const {
	Vector<String> ret;
	ERR_FAIL_COND_V(p_idx < 0, ret);

	if (p_idx < nodes.size()) {
		// Find in built-in nodes.
		for (int i = 0; i < nodes[p_idx].properties.size(); i++) {
			uint32_t idx = nodes[p_idx].properties[i].name;
			if (idx & FLAG_PATH_PROPERTY_IS_NODE) {
				ret.push_back(names[idx & FLAG_PROP_NAME_MASK]);
			}
		}
		return ret;
	}

	// Property not found, try on instance.
	HashMap<int, int>::ConstIterator I = base_scene_node_remap.find(p_idx);
	if (I) {
		return get_base_scene_state()->get_node_deferred_nodepath_properties(I->value);
	}

	return ret;
}

Variant SceneState::get_node_property_value(int p_idx, int p_prop) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), Variant());
	ERR_FAIL_INDEX_V(p_prop, nodes[p_idx].properties.size(), Variant());

	return variants[nodes[p_idx].properties[p_prop].value];
}

NodePath SceneState::get_node_owner_path(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, nodes.size(), NodePath());
	if (nodes[p_idx].owner < 0 || nodes[p_idx].owner == NO_PARENT_SAVED) {
		return NodePath(); //root likely
	}
	if (nodes[p_idx].owner & FLAG_ID_IS_PATH) {
		return node_paths[nodes[p_idx].owner & FLAG_MASK];
	} else {
		return get_node_path(nodes[p_idx].owner & FLAG_MASK);
	}
}

int SceneState::get_connection_count() const {
	return connections.size();
}

NodePath SceneState::get_connection_source(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, connections.size(), NodePath());
	if (connections[p_idx].from & FLAG_ID_IS_PATH) {
		return node_paths[connections[p_idx].from & FLAG_MASK];
	} else {
		return get_node_path(connections[p_idx].from & FLAG_MASK);
	}
}

StringName SceneState::get_connection_signal(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, connections.size(), StringName());
	return names[connections[p_idx].signal];
}

NodePath SceneState::get_connection_target(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, connections.size(), NodePath());
	if (connections[p_idx].to & FLAG_ID_IS_PATH) {
		return node_paths[connections[p_idx].to & FLAG_MASK];
	} else {
		return get_node_path(connections[p_idx].to & FLAG_MASK);
	}
}

PackedInt32Array SceneState::get_connection_target_id_path(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, connections.size(), PackedInt32Array());
	if (connections[p_idx].to & FLAG_ID_IS_PATH && connections[p_idx].to < id_paths.size()) {
		return id_paths[connections[p_idx].to];
	} else {
		return PackedInt32Array();
	}
}

PackedInt32Array SceneState::get_connection_source_id_path(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, connections.size(), PackedInt32Array());
	if (connections[p_idx].from & FLAG_ID_IS_PATH && connections[p_idx].from < id_paths.size()) {
		return id_paths[connections[p_idx].from];
	} else {
		return PackedInt32Array();
	}
}

StringName SceneState::get_connection_method(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, connections.size(), StringName());
	return names[connections[p_idx].method];
}

int SceneState::get_connection_flags(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, connections.size(), -1);
	return connections[p_idx].flags;
}

int SceneState::get_connection_unbinds(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, connections.size(), -1);
	return connections[p_idx].unbinds;
}

Array SceneState::get_connection_binds(int p_idx) const {
	ERR_FAIL_INDEX_V(p_idx, connections.size(), Array());
	Array binds;
	for (int i = 0; i < connections[p_idx].binds.size(); i++) {
		binds.push_back(variants[connections[p_idx].binds[i]]);
	}
	return binds;
}

bool SceneState::has_connection(const NodePath &p_node_from, const StringName &p_signal, const NodePath &p_node_to, const StringName &p_method, bool p_no_inheritance) {
	// this method cannot be const because of this
	Ref<SceneState> ss = this;

	do {
		for (int i = 0; i < ss->connections.size(); i++) {
			const ConnectionData &c = ss->connections[i];

			NodePath np_from;

			if (c.from & FLAG_ID_IS_PATH) {
				np_from = ss->node_paths[c.from & FLAG_MASK];
			} else {
				np_from = ss->get_node_path(c.from);
			}

			NodePath np_to;

			if (c.to & FLAG_ID_IS_PATH) {
				np_to = ss->node_paths[c.to & FLAG_MASK];
			} else {
				np_to = ss->get_node_path(c.to);
			}

			StringName sn_signal = ss->names[c.signal];
			StringName sn_method = ss->names[c.method];

			if (np_from == p_node_from && sn_signal == p_signal && np_to == p_node_to && sn_method == p_method) {
				return true;
			}
		}

		if (p_no_inheritance) {
			break;
		}

		ss = ss->get_base_scene_state();
	} while (ss.is_valid());

	return false;
}

Vector<NodePath> SceneState::get_editable_instances() const {
	return editable_instances;
}

Vector<NodePath> SceneState::get_exposed_children() const {
	return exposed_children;
}

Ref<Resource> SceneState::get_sub_resource(const String &p_path) {
	for (const Variant &v : variants) {
		const Ref<Resource> &res = v;
		if (res.is_valid() && res->get_path() == p_path) {
			return res;
		}
	}
	return Ref<Resource>();
}

Vector<Ref<Resource>> SceneState::get_sub_resources() {
	const String path_prefix = get_path() + "::";
	Vector<Ref<Resource>> sub_resources;
	for (const Variant &v : variants) {
		const Ref<Resource> &res = v;
		if (res.is_valid() && res->get_path().begins_with(path_prefix)) {
			sub_resources.push_back(res);
		}
	}
	return sub_resources;
}

//add

int SceneState::add_name(const StringName &p_name) {
	names.push_back(p_name);
	return names.size() - 1;
}

int SceneState::add_value(const Variant &p_value) {
	variants.push_back(p_value);
	return variants.size() - 1;
}

int SceneState::add_node_path(const NodePath &p_path, const PackedInt32Array &p_uid_path) {
	node_paths.push_back(p_path);
	id_paths.push_back(p_uid_path);
	return (node_paths.size() - 1) | FLAG_ID_IS_PATH;
}

int SceneState::add_node(int p_parent, int p_owner, int p_type, int p_name, int p_instance, int p_index, int32_t p_unique_id) {
	NodeData nd;
	nd.parent = p_parent;
	nd.owner = p_owner;
	nd.type = p_type;
	nd.name = p_name;
	nd.instance = p_instance;
	nd.index = p_index;

	nodes.push_back(nd);

	ids.push_back(p_unique_id);

	return nodes.size() - 1;
}

void SceneState::add_node_property(int p_node, int p_name, int p_value, bool p_deferred_node_path) {
	ERR_FAIL_INDEX(p_node, nodes.size());
	ERR_FAIL_INDEX(p_name, names.size());
	ERR_FAIL_INDEX(p_value, variants.size());

	NodeData::Property prop;
	prop.name = p_name;
	if (p_deferred_node_path) {
		prop.name |= FLAG_PATH_PROPERTY_IS_NODE;
	}
	prop.value = p_value;
	nodes.write[p_node].properties.push_back(prop);
}

void SceneState::add_node_group(int p_node, int p_group) {
	ERR_FAIL_INDEX(p_node, nodes.size());
	ERR_FAIL_INDEX(p_group, names.size());
	nodes.write[p_node].groups.push_back(p_group);
}

void SceneState::set_base_scene(int p_idx) {
	ERR_FAIL_INDEX(p_idx, variants.size());
	base_scene_idx = p_idx;
}

void SceneState::add_connection(int p_from, int p_to, int p_signal, int p_method, int p_flags, int p_unbinds, const Vector<int> &p_binds) {
	ERR_FAIL_INDEX(p_signal, names.size());
	ERR_FAIL_INDEX(p_method, names.size());

	for (int i = 0; i < p_binds.size(); i++) {
		ERR_FAIL_INDEX(p_binds[i], variants.size());
	}
	ConnectionData c;
	c.from = p_from;
	c.to = p_to;
	c.signal = p_signal;
	c.method = p_method;
	c.flags = p_flags;
	c.unbinds = p_unbinds;
	c.binds = p_binds;
	connections.push_back(c);
}

void SceneState::add_editable_instance(const NodePath &p_path) {
	editable_instances.push_back(p_path);
}

void SceneState::add_exposed_child(const NodePath &p_path) {
	exposed_children.push_back(p_path);
}

bool SceneState::remove_group_references(const StringName &p_name) {
	bool edited = false;
	for (NodeData &node : nodes) {
		for (const int &group : node.groups) {
			if (names[group] == p_name) {
				node.groups.erase(group);
				edited = true;
				break;
			}
		}
	}
	return edited;
}

bool SceneState::rename_group_references(const StringName &p_old_name, const StringName &p_new_name) {
	bool edited = false;
	for (const NodeData &node : nodes) {
		for (const int &group : node.groups) {
			if (names[group] == p_old_name) {
				names.write[group] = p_new_name;
				edited = true;
				break;
			}
		}
	}
	return edited;
}

HashSet<StringName> SceneState::get_all_groups() {
	HashSet<StringName> ret;
	for (const NodeData &node : nodes) {
		for (const int &group : node.groups) {
			ret.insert(names[group]);
		}
	}
	return ret;
}

Vector<String> SceneState::_get_node_groups(int p_idx) const {
	Vector<StringName> groups = get_node_groups(p_idx);
	Vector<String> ret;

	for (int i = 0; i < groups.size(); i++) {
		ret.push_back(groups[i]);
	}

	return ret;
}

void SceneState::_bind_methods() {
	//unbuild API

	ClassDB::bind_method(D_METHOD("get_path"), &SceneState::get_path);
	ClassDB::bind_method(D_METHOD("get_base_scene_state"), &SceneState::get_base_scene_state);
	ClassDB::bind_method(D_METHOD("get_node_count"), &SceneState::get_node_count);
	ClassDB::bind_method(D_METHOD("get_node_type", "idx"), &SceneState::get_node_type);
	ClassDB::bind_method(D_METHOD("get_node_name", "idx"), &SceneState::get_node_name);
	ClassDB::bind_method(D_METHOD("get_node_path", "idx", "for_parent"), &SceneState::get_node_path, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_node_owner_path", "idx"), &SceneState::get_node_owner_path);
	ClassDB::bind_method(D_METHOD("is_node_instance_placeholder", "idx"), &SceneState::is_node_instance_placeholder);
	ClassDB::bind_method(D_METHOD("get_node_instance_placeholder", "idx"), &SceneState::get_node_instance_placeholder);
	ClassDB::bind_method(D_METHOD("get_node_instance", "idx"), &SceneState::get_node_instance);
	ClassDB::bind_method(D_METHOD("get_node_groups", "idx"), &SceneState::_get_node_groups);
	ClassDB::bind_method(D_METHOD("get_node_index", "idx"), &SceneState::get_node_index);
	ClassDB::bind_method(D_METHOD("get_node_property_count", "idx"), &SceneState::get_node_property_count);
	ClassDB::bind_method(D_METHOD("get_node_property_name", "idx", "prop_idx"), &SceneState::get_node_property_name);
	ClassDB::bind_method(D_METHOD("get_node_property_value", "idx", "prop_idx"), &SceneState::get_node_property_value);
	ClassDB::bind_method(D_METHOD("get_connection_count"), &SceneState::get_connection_count);
	ClassDB::bind_method(D_METHOD("get_connection_source", "idx"), &SceneState::get_connection_source);
	ClassDB::bind_method(D_METHOD("get_connection_signal", "idx"), &SceneState::get_connection_signal);
	ClassDB::bind_method(D_METHOD("get_connection_target", "idx"), &SceneState::get_connection_target);
	ClassDB::bind_method(D_METHOD("get_connection_method", "idx"), &SceneState::get_connection_method);
	ClassDB::bind_method(D_METHOD("get_connection_flags", "idx"), &SceneState::get_connection_flags);
	ClassDB::bind_method(D_METHOD("get_connection_binds", "idx"), &SceneState::get_connection_binds);
	ClassDB::bind_method(D_METHOD("get_connection_unbinds", "idx"), &SceneState::get_connection_unbinds);

	BIND_ENUM_CONSTANT(GEN_EDIT_STATE_DISABLED);
	BIND_ENUM_CONSTANT(GEN_EDIT_STATE_INSTANCE);
	BIND_ENUM_CONSTANT(GEN_EDIT_STATE_MAIN);
	BIND_ENUM_CONSTANT(GEN_EDIT_STATE_MAIN_INHERITED);
}

void SceneInstantiationPlan::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_root_plan_id"), &SceneInstantiationPlan::get_root_plan_id);
	ClassDB::bind_method(D_METHOD("get_root_node"), &SceneInstantiationPlan::get_root_node);
}

void SceneInstantiationPlanNode::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_plan_id"), &SceneInstantiationPlanNode::get_plan_id);
	ClassDB::bind_method(D_METHOD("get_name"), &SceneInstantiationPlanNode::get_name);
	ClassDB::bind_method(D_METHOD("set_name", "name"), &SceneInstantiationPlanNode::set_name);
	ClassDB::bind_method(D_METHOD("get_type"), &SceneInstantiationPlanNode::get_type);
	ClassDB::bind_method(D_METHOD("get_source_path"), &SceneInstantiationPlanNode::get_source_path);
	ClassDB::bind_method(D_METHOD("get_source_scene_path"), &SceneInstantiationPlanNode::get_source_scene_path);
	ClassDB::bind_method(D_METHOD("get_origin"), &SceneInstantiationPlanNode::get_origin);
	ClassDB::bind_method(D_METHOD("is_instance_root"), &SceneInstantiationPlanNode::is_instance_root);
	ClassDB::bind_method(D_METHOD("get_parent"), &SceneInstantiationPlanNode::get_parent);
	ClassDB::bind_method(D_METHOD("get_child_count"), &SceneInstantiationPlanNode::get_child_count);
	ClassDB::bind_method(D_METHOD("get_child", "index"), &SceneInstantiationPlanNode::get_child);
	ClassDB::bind_method(D_METHOD("get_children"), &SceneInstantiationPlanNode::get_children);
	ClassDB::bind_method(D_METHOD("has_property", "name"), &SceneInstantiationPlanNode::has_property);
	ClassDB::bind_method(D_METHOD("get_property", "name", "default_value"), &SceneInstantiationPlanNode::get_property, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("set_property", "name", "value"), &SceneInstantiationPlanNode::set_property);
	ClassDB::bind_method(D_METHOD("clear_property_override", "name"), &SceneInstantiationPlanNode::clear_property_override);
	ClassDB::bind_method(D_METHOD("prune"), &SceneInstantiationPlanNode::prune);
	ClassDB::bind_method(D_METHOD("duplicate", "additional_count"), &SceneInstantiationPlanNode::duplicate);
	ClassDB::bind_method(D_METHOD("extract_scene"), &SceneInstantiationPlanNode::extract_scene);
	ClassDB::bind_method(D_METHOD("flatten_into_parent"), &SceneInstantiationPlanNode::flatten_into_parent);

	BIND_ENUM_CONSTANT(ORIGIN_LOCAL);
	BIND_ENUM_CONSTANT(ORIGIN_NESTED_SCENE);
	BIND_ENUM_CONSTANT(ORIGIN_OWNER_ADDED);
	BIND_ENUM_CONSTANT(ORIGIN_DUPLICATED);
}

SceneState::SceneState() {
}

////////////////

void PackedScene::_set_bundled_scene(const Dictionary &p_scene) {
	state->set_bundled_scene(p_scene);
}

Dictionary PackedScene::_get_bundled_scene() const {
	return state->get_bundled_scene();
}

Error PackedScene::pack(Node *p_scene) {
	return state->pack(p_scene);
}

void PackedScene::clear() {
	state->clear();
}

void PackedScene::reload_from_file() {
	String path = get_path();
	if (!path.is_resource_file()) {
		return;
	}

	Ref<PackedScene> s = ResourceLoader::load(ResourceLoader::path_remap(path), get_class(), ResourceFormatLoader::CACHE_MODE_IGNORE);
	if (s.is_null()) {
		return;
	}

	// Backup the loaded_state
	Ref<SceneState> loaded_state = s->get_state();
	// This assigns a new state to s->state
	// We do this because of the next step
	s->recreate_state();
	// This has a side-effect to clear s->state
	copy_from(s);
	// Then, we copy the backed-up loaded_state to state
	state->copy_from(loaded_state);
}

bool PackedScene::can_instantiate() const {
	return state->can_instantiate();
}

Node *PackedScene::instantiate(GenEditState p_edit_state) const {
#ifndef TOOLS_ENABLED
	ERR_FAIL_COND_V_MSG(p_edit_state != GEN_EDIT_STATE_DISABLED, nullptr, "Edit state is only for editors, does not work without tools compiled.");
#endif
	Node *s = state->instantiate((SceneState::GenEditState)p_edit_state);
	if (!s) {
		return nullptr;
	}

	if (p_edit_state != GEN_EDIT_STATE_DISABLED) {
		s->set_scene_instance_state(state);
	}

	if (!is_built_in()) {
		s->set_scene_file_path(get_path());
	}

	s->notification(Node::NOTIFICATION_SCENE_INSTANTIATED);

	return s;
}

void PackedScene::replace_state(Ref<SceneState> p_by) {
	state = p_by;
	state->set_path(get_path());
#ifdef TOOLS_ENABLED
	state->set_last_modified_time(get_last_modified_time());
#endif
}

void PackedScene::recreate_state() {
	state.instantiate();
	state->set_path(get_path());
#ifdef TOOLS_ENABLED
	state->set_last_modified_time(get_last_modified_time());
#endif
}

#ifdef TOOLS_ENABLED
HashSet<StringName> PackedScene::get_scene_groups(const String &p_path) {
	{
		Ref<PackedScene> packed_scene = ResourceCache::get_ref(p_path);
		if (packed_scene.is_valid()) {
			return packed_scene->get_state()->get_all_groups();
		}
	}

	if (p_path.get_extension() == "tscn") {
		Ref<FileAccess> scene_file = FileAccess::open(p_path, FileAccess::READ);
		ERR_FAIL_COND_V(scene_file.is_null(), HashSet<StringName>());

		HashSet<StringName> ret;
		while (!scene_file->eof_reached()) {
			const String line = scene_file->get_line();
			if (!line.begins_with("[node")) {
				continue;
			}

			int i = line.find("groups=[");
			if (i == -1) {
				continue;
			}

			int j = line.find_char(']', i);
			while (i < j) {
				i = line.find_char('"', i);
				if (i == -1) {
					break;
				}

				int k = line.find_char('"', i + 1);
				if (k == -1) {
					break;
				}

				ret.insert(line.substr(i + 1, k - i - 1));
				i = k + 1;
			}
		}
		return ret;
	} else {
		Ref<PackedScene> packed_scene = ResourceLoader::load(p_path);
		ERR_FAIL_COND_V(packed_scene.is_null(), HashSet<StringName>());
		return packed_scene->get_state()->get_all_groups();
	}
}
#endif

Ref<SceneState> PackedScene::get_state() const {
	return state;
}

void PackedScene::set_path(const String &p_path, bool p_take_over) {
	state->set_path(p_path);
	Resource::set_path(p_path, p_take_over);
}

void PackedScene::set_path_cache(const String &p_path) {
	state->set_path(p_path);
	Resource::set_path_cache(p_path);
}

void PackedScene::reset_state() {
	clear();
}
void PackedScene::_bind_methods() {
	ClassDB::bind_method(D_METHOD("pack", "path"), &PackedScene::pack);
	ClassDB::bind_method(D_METHOD("instantiate", "edit_state"), &PackedScene::instantiate, DEFVAL(GEN_EDIT_STATE_DISABLED));
	ClassDB::bind_method(D_METHOD("can_instantiate"), &PackedScene::can_instantiate);
	ClassDB::bind_method(D_METHOD("_set_bundled_scene", "scene"), &PackedScene::_set_bundled_scene);
	ClassDB::bind_method(D_METHOD("_get_bundled_scene"), &PackedScene::_get_bundled_scene);
	ClassDB::bind_method(D_METHOD("get_state"), &PackedScene::get_state);

	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "_bundled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL), "_set_bundled_scene", "_get_bundled_scene");

	BIND_ENUM_CONSTANT(GEN_EDIT_STATE_DISABLED);
	BIND_ENUM_CONSTANT(GEN_EDIT_STATE_INSTANCE);
	BIND_ENUM_CONSTANT(GEN_EDIT_STATE_MAIN);
	BIND_ENUM_CONSTANT(GEN_EDIT_STATE_MAIN_INHERITED);
}

PackedScene::PackedScene() {
	state.instantiate();
}
