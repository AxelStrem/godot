/**************************************************************************/
/*  wfc_types.h                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/packed_scene.h"

class WFCCatalog : public Resource {
	GDCLASS(WFCCatalog, Resource);

	Dictionary rules;

protected:
	static void _bind_methods();

public:
	void set_rules(const Dictionary &p_rules);
	Dictionary get_rules() const;
};

class WFCCatalogSet : public Resource {
	GDCLASS(WFCCatalogSet, Resource);

	TypedArray<WFCCatalog> catalogs;

protected:
	static void _bind_methods();

public:
	void set_catalogs(const TypedArray<WFCCatalog> &p_catalogs);
	TypedArray<WFCCatalog> get_catalogs() const;
	Dictionary merge_rules() const;
};

class WFCParam : public Resource {
	GDCLASS(WFCParam, Resource);

	StringName option;
	float probability = 1.0f;
	bool enabled = true;
	Ref<PackedScene> scene;
	int symmetry_fold = 1;

protected:
	static void _bind_methods();

public:
	void set_option(const StringName &p_option);
	StringName get_option() const;

	void set_probability(float p_probability);
	float get_probability() const;

	void set_enabled(bool p_enabled);
	bool is_enabled() const;

	void set_scene(const Ref<PackedScene> &p_scene);
	Ref<PackedScene> get_scene() const;

	void set_symmetry_fold(int p_symmetry_fold);
	int get_symmetry_fold() const;
};

class WFCNeighbor : public Resource {
	GDCLASS(WFCNeighbor, Resource);

	StringName name;
	StringName inv_name;
	StringName type;
	Vector3 offset;
	float wobble = 0.001f;
	Vector3 angle;
	Vector3 angular_wobble = Vector3(0.5, 0.5, 0.5);
	StringName connection;
	bool primary = true;
	StringName rotation_lock;

protected:
	static void _bind_methods();

public:
	void set_side_name(const StringName &p_name);
	StringName get_side_name() const;

	void set_inv_name(const StringName &p_name);
	StringName get_inv_name() const;

	void set_type(const StringName &p_type);
	StringName get_type() const;

	void set_offset(const Vector3 &p_offset);
	Vector3 get_offset() const;

	void set_wobble(float p_wobble);
	float get_wobble() const;

	void set_angle(const Vector3 &p_angle);
	Vector3 get_angle() const;

	void set_angular_wobble(const Vector3 &p_wobble);
	Vector3 get_angular_wobble() const;

	void set_connection(const StringName &p_connection);
	StringName get_connection() const;

	void set_primary(bool p_primary);
	bool is_primary() const;

	void set_rotation_lock(const StringName &p_rotation_lock);
	StringName get_rotation_lock() const;
};

class WFCElement : public Node3D {
	GDCLASS(WFCElement, Node3D);

	StringName type;
	TypedArray<WFCParam> options;
	TypedArray<WFCNeighbor> neighbor_points;
	StringName selected_option;
	int resolve_priority = 0;
	bool defer_collapse = false;
	Dictionary resolved_data;
	HashMap<StringName, ObjectID> connected_neighbors;
	Vector<ObjectID> materialized_children;

protected:
	static void _bind_methods();
	GDVIRTUAL0(_post_materialize);

public:
	void set_type(const StringName &p_type);
	StringName get_type() const;

	void set_options(const Array &p_options);
	TypedArray<WFCParam> get_options() const;

	void set_neighbor_points(const Array &p_neighbor_points);
	TypedArray<WFCNeighbor> get_neighbor_points() const;

	void set_selected_option(const StringName &p_selected_option);
	StringName get_selected_option() const;
	void set_resolve_priority(int p_resolve_priority);
	int get_resolve_priority() const;
	void set_defer_collapse(bool p_defer_collapse);
	bool is_defer_collapse_enabled() const;
	void set_resolved_data(const Dictionary &p_resolved_data);
	Dictionary get_resolved_data() const;

	PackedStringArray get_enabled_options() const;
	void set_enabled_options(const PackedStringArray &p_enabled_options);
	void clear_connected_neighbors();
	void set_connected_neighbor(const StringName &p_side_name, WFCElement *p_element);
	bool has_connected_neighbor(const StringName &p_side_name) const;
	WFCElement *get_connected_neighbor(const StringName &p_side_name) const;
	bool apply_selected_option(const StringName &p_option);
	void clear_materialized();
	void materialize();
	void post_materialize();
};