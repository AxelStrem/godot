/**************************************************************************/
/*  wfc_graph.h                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "core/variant/variant.h"

class WFCGraphElement : public RefCounted {
	GDCLASS(WFCGraphElement, RefCounted);

	int64_t id = 0;
	StringName type;
	Transform3D global_transform;
	PackedStringArray allowed_options;
	StringName selected_option;
	Dictionary neighbors;
	int resolve_priority = 0;
	bool defer_collapse = false;
	Dictionary resolved_data;

	void _sync_selected_option();

protected:
	static void _bind_methods();

public:
	void set_id(int64_t p_id);
	int64_t get_id() const;

	void set_type(const StringName &p_type);
	StringName get_type() const;

	void set_global_transform(const Transform3D &p_transform);
	Transform3D get_global_transform() const;

	void set_allowed_options(const PackedStringArray &p_options);
	PackedStringArray get_allowed_options() const;
	bool has_option(const StringName &p_option) const;
	void disable_option(const StringName &p_option);
	void keep_only_options(const PackedStringArray &p_options);

	void set_selected_option(const StringName &p_option);
	StringName get_selected_option() const;

	void set_neighbors(const Dictionary &p_neighbors);
	Dictionary get_neighbors() const;
	void set_neighbor_id(const StringName &p_side_name, int64_t p_neighbor_id);
	bool has_neighbor(const StringName &p_side_name) const;
	int64_t get_neighbor_id(const StringName &p_side_name) const;

	void set_resolve_priority(int p_priority);
	int get_resolve_priority() const;

	void set_defer_collapse(bool p_defer_collapse);
	bool is_defer_collapse_enabled() const;

	void set_resolved_data(const Dictionary &p_resolved_data);
	Dictionary get_resolved_data() const;
};

class WFCGraph : public RefCounted {
	GDCLASS(WFCGraph, RefCounted);

	Array elements;
	HashMap<int64_t, Ref<WFCGraphElement>> elements_by_id;
	uint64_t seed = 0;

protected:
	static void _bind_methods();

public:
	void set_seed(uint64_t p_seed);
	uint64_t get_seed() const;

	Array get_elements() const;
	Array get_elements_of_type(const StringName &p_type) const;
	Ref<WFCGraphElement> get_element(int64_t p_id) const;
	int get_element_count() const;

	void clear();
	void add_element(const Ref<WFCGraphElement> &p_element);
};

class WFCGraphProcessor : public Resource {
	GDCLASS(WFCGraphProcessor, Resource);

protected:
	static void _bind_methods();
	GDVIRTUAL1(_pre_resolve, Ref<WFCGraph>)
	GDVIRTUAL1(_post_resolve, Ref<WFCGraph>)

public:
	void pre_resolve(const Ref<WFCGraph> &p_graph);
	void post_resolve(const Ref<WFCGraph> &p_graph);
};