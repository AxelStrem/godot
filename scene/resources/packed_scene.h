/**************************************************************************/
/*  packed_scene.h                                                        */
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

#pragma once

#include "core/io/resource.h"
#include "scene/main/node.h"

class PackedScene;
class SceneInstantiationPlan;
class SceneInstantiationPlanNode;

enum SceneInstantiationPlanNodeOrigin {
	SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_LOCAL,
	SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_NESTED_SCENE,
	SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_OWNER_ADDED,
	SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_DUPLICATED,
};

class SceneState : public RefCounted {
	GDCLASS(SceneState, RefCounted);
	friend class SceneInstantiationPlan;

	Vector<StringName> names;
	Vector<Variant> variants;
	Vector<NodePath> node_paths;
	Vector<PackedInt32Array> id_paths;
	mutable PackedInt32Array ids;
	Vector<NodePath> editable_instances;
	Vector<NodePath> exposed_children;
	mutable HashMap<NodePath, int> node_path_cache;
	mutable HashMap<int, int> base_scene_node_remap;

	int base_scene_idx = -1;

	enum {
		NO_PARENT_SAVED = 0x7FFFFFFF,
		NAME_INDEX_BITS = 18,
		NAME_MASK = (1 << NAME_INDEX_BITS) - 1,
	};

	struct NodeData {
		int parent = 0;
		int owner = 0;
		int type = 0;
		int name = 0;
		int instance = 0;
		int index = 0;

		struct Property {
			int name = 0;
			int value = 0;
		};

		Vector<Property> properties;
		Vector<int> groups;
	};

	struct DeferredNodePathProperties {
		ObjectID base;
		StringName property;
		Variant value;
	};

	Vector<NodeData> nodes;

	struct ConnectionData {
		int from = 0;
		int to = 0;
		int signal = 0;
		int method = 0;
		int flags = 0;
		int unbinds = 0;
		Vector<int> binds;
	};

	Vector<ConnectionData> connections;

	Error _parse_node(Node *p_owner, Node *p_node, int p_parent_idx, HashMap<StringName, int> &name_map, HashMap<Variant, int> &variant_map, HashMap<Node *, int> &node_map, HashMap<Node *, int> &nodepath_map, HashSet<int32_t> &ids_saved);
	Error _parse_connections(Node *p_owner, Node *p_node, HashMap<StringName, int> &name_map, HashMap<Variant, int> &variant_map, HashMap<Node *, int> &node_map, HashMap<Node *, int> &nodepath_map);

	String path;

	uint64_t last_modified_time = 0;

	static bool disable_placeholders;

	Vector<String> _get_node_groups(int p_idx) const;

	int _find_base_scene_node_remap_key(int p_idx) const;

	Node *_recover_node_path_index(Node *p_base, int p_idx) const;

	static Variant _duplicate_recursive(const Variant &p_variant, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_remap_cache, const Variant &p_fallback, Node *p_for_scene);

#ifdef TOOLS_ENABLED
public:
	typedef void (*InstantiationWarningNotify)(const String &p_warning);

private:
	static InstantiationWarningNotify instantiation_warn_notify;
#endif

protected:
	static void _bind_methods();

public:
	enum {
		FLAG_ID_IS_PATH = (1 << 30),
		TYPE_INSTANTIATED = 0x7FFFFFFF,
		FLAG_INSTANCE_IS_PLACEHOLDER = (1 << 30),
		FLAG_PATH_PROPERTY_IS_NODE = (1 << 30),
		FLAG_PROP_NAME_MASK = FLAG_PATH_PROPERTY_IS_NODE - 1,
		FLAG_MASK = (1 << 24) - 1,
	};

	enum GenEditState {
		GEN_EDIT_STATE_DISABLED,
		GEN_EDIT_STATE_INSTANCE,
		GEN_EDIT_STATE_MAIN,
		GEN_EDIT_STATE_MAIN_INHERITED,
	};

	struct PackState {
		Ref<SceneState> state;
		int node = -1;
	};

	struct RuntimePlanSourceStateCache {
		Vector<NodePath> node_paths;
		Vector<NodePath> parent_paths;
		Vector<NodePath> owner_paths;
		Vector<Vector<int>> direct_children;
		Vector<Vector<int>> override_children;
	};

private:
	Node *_instantiate_legacy(GenEditState p_edit_state) const;
	Node *_instantiate_runtime_plan(GenEditState p_edit_state) const;
	Ref<SceneInstantiationPlan> _build_runtime_plan() const;
	const RuntimePlanSourceStateCache *_get_runtime_plan_source_state_cache(const Ref<SceneState> &p_source_state, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches) const;
	NodePath _get_runtime_plan_node_path(const Ref<SceneState> &p_source_state, int p_source_node_idx, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches, bool p_for_parent = false) const;
	NodePath _get_runtime_plan_owner_path(const Ref<SceneState> &p_source_state, int p_source_node_idx, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches) const;
	Vector<int> _get_runtime_plan_direct_children(const Ref<SceneState> &p_source_state, int p_source_node_idx, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches) const;
	Vector<int> _get_runtime_plan_override_children(const Ref<SceneState> &p_source_state, int p_source_node_idx, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches) const;
	bool _runtime_plan_path_is_descendant(const NodePath &p_ancestor_path, const NodePath &p_descendant_path) const;
	NodePath _get_runtime_plan_parent_path(const NodePath &p_path) const;
	NodePath _compose_runtime_plan_path(const NodePath &p_base_path, const NodePath &p_relative_path) const;
	void _copy_runtime_plan_base_properties(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, const Ref<SceneState> &p_source_state, int p_source_node_idx) const;
	void _apply_runtime_plan_property_overrides(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, const Ref<SceneState> &p_override_state, int p_override_node_idx) const;
	void _resolve_runtime_plan_deferred_node_paths(const Vector<DeferredNodePathProperties> &p_deferred_node_paths) const;
	Node *_get_runtime_plan_local_resource_base(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, Node *p_node, Node *p_root, Node *p_source_root) const;
	Variant _make_runtime_plan_local_resource(Variant &p_value, const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_property_name, Node *p_root, Node *p_source_root, GenEditState p_edit_state) const;
	Array _setup_runtime_plan_resources_in_array(Array &p_array_to_scan, const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_property_name, Node *p_root, Node *p_source_root, GenEditState p_edit_state) const;
	Dictionary _setup_runtime_plan_resources_in_dictionary(Dictionary &p_dictionary_to_scan, const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_property_name, Node *p_root, Node *p_source_root, GenEditState p_edit_state) const;
	int _find_runtime_plan_node_by_source_path(const Ref<SceneInstantiationPlan> &p_runtime_plan, const NodePath &p_source_path) const;
	void _merge_runtime_plan_instance_overrides(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, const Ref<SceneState> &p_override_state, int p_override_node_idx, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches) const;
	int _append_runtime_plan_node(const Ref<SceneInstantiationPlan> &p_runtime_plan, const Ref<SceneState> &p_source_state, int p_source_node_idx, int p_parent_plan_id, SceneInstantiationPlanNodeOrigin p_origin, HashMap<const SceneState *, RuntimePlanSourceStateCache> &r_source_state_caches, const NodePath &p_source_path_override = NodePath(), const StringName &p_name_override = StringName()) const;
	bool _runtime_plan_requires_legacy_connection_fallback(const Ref<SceneInstantiationPlan> &p_runtime_plan) const;
	void _apply_runtime_plan_connections(const Ref<SceneInstantiationPlan> &p_runtime_plan, const HashMap<int, ObjectID> &p_materialized_plan_nodes, GenEditState p_edit_state) const;
	bool _runtime_plan_requires_legacy_fallback(const Ref<SceneInstantiationPlan> &p_runtime_plan) const;
	bool _runtime_plan_uses_customization(const Ref<SceneInstantiationPlan> &p_runtime_plan) const;
	void _apply_legacy_filter_to_runtime_plan(Node *p_node, const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id) const;
	Node *_materialize_runtime_plan_node(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id, Node *p_parent, Node *p_root, Node *p_source_root, Vector<DeferredNodePathProperties> *p_deferred_node_paths, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> *p_resources_local_to_scenes, HashMap<int, ObjectID> *p_materialized_plan_nodes, GenEditState p_edit_state, bool p_apply_customization = true) const;
	NodePath _rebase_runtime_plan_subtree_path(const NodePath &p_root_source_path, const NodePath &p_path, bool p_fallback_to_root) const;
	int _clone_runtime_plan_subtree(const Ref<SceneInstantiationPlan> &p_source_plan, int p_source_plan_id, const Ref<SceneInstantiationPlan> &p_target_plan, int p_target_parent_plan_id, const NodePath &p_root_source_path) const;
	Ref<PackedScene> _extract_runtime_plan_scene(const Ref<SceneInstantiationPlan> &p_runtime_plan, int p_plan_id) const;

public:

	static void set_disable_placeholders(bool p_disable);
	static Ref<Resource> get_remap_resource(const Ref<Resource> &p_resource, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &remap_cache, const Ref<Resource> &p_fallback, Node *p_for_scene);

	int find_node_by_path(const NodePath &p_node) const;
	Variant get_property_value(int p_node, const StringName &p_property, bool &r_found, bool &r_node_deferred) const;
	bool is_node_in_group(int p_node, const StringName &p_group) const;
	bool is_connection(int p_node, const StringName &p_signal, int p_to_node, const StringName &p_to_method) const;

	void set_bundled_scene(const Dictionary &p_dictionary);
	Dictionary get_bundled_scene() const;

	Error pack(Node *p_scene);

	void set_path(const String &p_path);
	String get_path() const;

	void clear();
	Error copy_from(const Ref<SceneState> &p_scene_state);

	bool can_instantiate() const;
	Node *instantiate(GenEditState p_edit_state) const;

	Array setup_resources_in_array(Array &array_to_scan, const SceneState::NodeData &n, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *node, const StringName sname, int i, Node **ret_nodes, SceneState::GenEditState p_edit_state) const;
	Dictionary setup_resources_in_dictionary(Dictionary &p_dictionary_to_scan, const SceneState::NodeData &p_n, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_sname, int p_i, Node **p_ret_nodes, SceneState::GenEditState p_edit_state) const;

	// Helper methods for node duplication
	Node *create_duplicate_node(Node *original_node, const NodeData &n, const StringName *snames, const Variant *props, GenEditState p_edit_state) const;
	String build_node_path(int node_idx, const Vector<NodeData> &nodes, const StringName *snames, const Vector<NodePath> &node_paths) const;
	Variant make_local_resource(Variant &value, const SceneState::NodeData &p_node_data, HashMap<Node *, HashMap<Ref<Resource>, Ref<Resource>>> &p_resources_local_to_scenes, Node *p_node, const StringName p_sname, int p_i, Node **p_ret_nodes, SceneState::GenEditState p_edit_state) const;
	bool has_local_resource(const Array &p_array) const;

	Ref<SceneState> get_base_scene_state() const;

	void update_instance_resource(String p_path, Ref<PackedScene> p_packed_scene);

	//unbuild API

	int get_node_count() const;
	StringName get_node_type(int p_idx) const;
	StringName get_node_name(int p_idx) const;
	NodePath get_node_path(int p_idx, bool p_for_parent = false) const;
	int32_t get_node_unique_id(int p_idx) const;
	PackedInt32Array get_node_id_path(int p_idx) const;
	PackedInt32Array get_node_parent_id_path(int p_idx) const;
	NodePath get_node_owner_path(int p_idx) const;
	PackedInt32Array get_node_owner_id_path(int p_idx) const;
	Ref<PackedScene> get_node_instance(int p_idx) const;
	String get_node_instance_placeholder(int p_idx) const;
	bool is_node_instance_placeholder(int p_idx) const;
	Vector<StringName> get_node_groups(int p_idx) const;
	int get_node_index(int p_idx) const;

	int get_node_property_count(int p_idx) const;
	StringName get_node_property_name(int p_idx, int p_prop) const;
	Variant get_node_property_value(int p_idx, int p_prop) const;
	Vector<String> get_node_deferred_nodepath_properties(int p_idx) const;

	int get_connection_count() const;
	NodePath get_connection_source(int p_idx) const;
	StringName get_connection_signal(int p_idx) const;
	NodePath get_connection_target(int p_idx) const;
	StringName get_connection_method(int p_idx) const;

	PackedInt32Array get_connection_source_id_path(int p_idx) const;
	PackedInt32Array get_connection_target_id_path(int p_idx) const;

	int get_connection_flags(int p_idx) const;
	int get_connection_unbinds(int p_idx) const;
	Array get_connection_binds(int p_idx) const;

	bool has_connection(const NodePath &p_node_from, const StringName &p_signal, const NodePath &p_node_to, const StringName &p_method, bool p_no_inheritance = false);

	Vector<NodePath> get_editable_instances() const;

	void add_exposed_child(const NodePath &p_path);
	Vector<NodePath> get_exposed_children() const;
	Ref<Resource> get_sub_resource(const String &p_path);
	Vector<Ref<Resource>> get_sub_resources();

	//build API

	int add_name(const StringName &p_name);
	int add_value(const Variant &p_value);
	int add_node_path(const NodePath &p_path, const PackedInt32Array &p_uid_path);
	int add_node(int p_parent, int p_owner, int p_type, int p_name, int p_instance, int p_index, int32_t p_unique_id);
	void add_node_property(int p_node, int p_name, int p_value, bool p_deferred_node_path = false);
	void add_node_group(int p_node, int p_group);
	void set_base_scene(int p_idx);
	void add_connection(int p_from, int p_to, int p_signal, int p_method, int p_flags, int p_unbinds, const Vector<int> &p_binds);
	void add_editable_instance(const NodePath &p_path);

	bool remove_group_references(const StringName &p_name);
	bool rename_group_references(const StringName &p_old_name, const StringName &p_new_name);
	HashSet<StringName> get_all_groups();

	virtual void set_last_modified_time(uint64_t p_time) { last_modified_time = p_time; }
	uint64_t get_last_modified_time() const { return last_modified_time; }

	// Used when saving pointers (saves a path property instead).
	static String get_meta_pointer_property(const String &p_property);

#ifdef TOOLS_ENABLED
	static void set_instantiation_warning_notify_func(InstantiationWarningNotify p_warn_notify) { instantiation_warn_notify = p_warn_notify; }
#endif

	SceneState();
};

VARIANT_ENUM_CAST(SceneState::GenEditState)

class SceneInstantiationPlan : public RefCounted {
	GDCLASS(SceneInstantiationPlan, RefCounted);

	friend class SceneInstantiationPlanNode;
	friend class SceneState;

	struct PlanNodeData {
		int plan_id = -1;
		int parent_plan_id = -1;
		Vector<int> child_plan_ids;
		Ref<SceneState> source_state;
		int source_node_idx = -1;
		Vector<SceneState::PackState> override_sources;
		NodePath source_path;
		NodePath owner_path;
		String source_scene_path;
		StringName name;
		StringName type;
		HashMap<StringName, Variant> base_properties;
		HashMap<StringName, Variant> properties;
		HashSet<StringName> deferred_node_properties;
		SceneInstantiationPlanNodeOrigin origin = SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_LOCAL;
		int duplicated_from_plan_id = -1;
		bool instance_root = false;
		bool pruned = false;
		bool flattened = false;
	};

	Vector<PlanNodeData> plan_nodes;
	HashMap<NodePath, HashSet<int>> source_path_index;
	int root_plan_id = -1;

	const PlanNodeData *_get_node_data(int p_plan_id) const;
	PlanNodeData *_get_node_data_w(int p_plan_id);
	void _index_source_path(int p_plan_id, const NodePath &p_source_path);
	void _unindex_source_path(int p_plan_id, const NodePath &p_source_path);
	int _find_node_by_source_path(const NodePath &p_source_path) const;
	Ref<SceneInstantiationPlanNode> _make_node_ref(int p_plan_id) const;
	void _detach_from_parent(int p_plan_id);
	int _duplicate_node_subtree(int p_plan_id, int p_parent_plan_id);
	void _set_node_property(int p_plan_id, const StringName &p_name, const Variant &p_value, bool p_deferred_node_path = false);
	void _clear_node_property_override(int p_plan_id, const StringName &p_name);
	void _rename_node(int p_plan_id, const StringName &p_name);
	void _prune_node(int p_plan_id);
	Array _duplicate_node(int p_plan_id, int p_additional_count);
	Ref<PackedScene> _extract_scene(int p_plan_id) const;
	void _flatten_node(int p_plan_id);

protected:
	static void _bind_methods();

public:
	int add_node(const Ref<SceneState> &p_source_state, int p_source_node_idx, int p_parent_plan_id, const NodePath &p_source_path, const String &p_source_scene_path, const StringName &p_name, const StringName &p_type, SceneInstantiationPlanNodeOrigin p_origin, bool p_instance_root);
	void set_root_plan_id(int p_plan_id);
	void set_node_base_property(int p_plan_id, const StringName &p_name, const Variant &p_value, bool p_deferred_node_path = false);
	bool has_node(int p_plan_id) const;
	int get_root_plan_id() const;
	Ref<SceneInstantiationPlanNode> get_root_node() const;

	SceneInstantiationPlan();
};

class SceneInstantiationPlanNode : public RefCounted {
	GDCLASS(SceneInstantiationPlanNode, RefCounted);

	friend class SceneInstantiationPlan;

	Ref<SceneInstantiationPlan> plan;
	int plan_id = -1;

	void _setup(const Ref<SceneInstantiationPlan> &p_plan, int p_plan_id);

protected:
	static void _bind_methods();

public:
	enum Origin {
		ORIGIN_LOCAL = SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_LOCAL,
		ORIGIN_NESTED_SCENE = SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_NESTED_SCENE,
		ORIGIN_OWNER_ADDED = SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_OWNER_ADDED,
		ORIGIN_DUPLICATED = SCENE_INSTANTIATION_PLAN_NODE_ORIGIN_DUPLICATED,
	};

	int get_plan_id() const;
	StringName get_name() const;
	void set_name(const StringName &p_name);
	StringName get_type() const;
	NodePath get_source_path() const;
	String get_source_scene_path() const;
	int get_origin() const;
	bool is_instance_root() const;
	Ref<SceneInstantiationPlanNode> get_parent() const;
	int get_child_count() const;
	Ref<SceneInstantiationPlanNode> get_child(int p_index) const;
	Array get_children() const;
	bool has_property(const StringName &p_name) const;
	Variant get_property(const StringName &p_name, const Variant &p_default = Variant()) const;
	void set_property(const StringName &p_name, const Variant &p_value);
	void clear_property_override(const StringName &p_name);
	void prune();
	Array duplicate(int p_additional_count);
	Ref<PackedScene> extract_scene() const;
	void flatten_into_parent();

	SceneInstantiationPlanNode();
};

VARIANT_ENUM_CAST(SceneInstantiationPlanNode::Origin)

class PackedScene : public Resource {
	GDCLASS(PackedScene, Resource);
	RES_BASE_EXTENSION("scn");

	Ref<SceneState> state;

	void _set_bundled_scene(const Dictionary &p_scene);
	Dictionary _get_bundled_scene() const;

protected:
	virtual bool editor_can_reload_from_file() override { return false; } // this is handled by editor better
	static void _bind_methods();
	virtual void reset_state() override;

public:
	enum GenEditState {
		GEN_EDIT_STATE_DISABLED,
		GEN_EDIT_STATE_INSTANCE,
		GEN_EDIT_STATE_MAIN,
		GEN_EDIT_STATE_MAIN_INHERITED,
	};

	Error pack(Node *p_scene);

	void clear();

	bool can_instantiate() const;
	Node *instantiate(GenEditState p_edit_state = GEN_EDIT_STATE_DISABLED) const;

	void recreate_state();
	void replace_state(Ref<SceneState> p_by);

	virtual void reload_from_file() override;

	virtual void set_path(const String &p_path, bool p_take_over = false) override;
	virtual void set_path_cache(const String &p_path) override;

#ifdef TOOLS_ENABLED
	virtual void set_last_modified_time(uint64_t p_time) override {
		Resource::set_last_modified_time(p_time);
		state->set_last_modified_time(p_time);
	}

	static HashSet<StringName> get_scene_groups(const String &p_path);
#endif
	Ref<SceneState> get_state() const;

	PackedScene();
};

VARIANT_ENUM_CAST(PackedScene::GenEditState)
