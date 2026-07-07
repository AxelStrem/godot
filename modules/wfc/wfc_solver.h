/**************************************************************************/
/*  wfc_solver.h                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/object/worker_thread_pool.h"
#include "core/string/ustring.h"
#include "core/templates/hash_set.h"
#include "scene/main/node.h"

#include "wfc_graph.h"
#include "wfc_types.h"

class WFCSolver : public Node {
	GDCLASS(WFCSolver, Node);

	struct AsyncJob;

	Ref<WFCCatalogSet> catalog_set;
	Ref<WFCGraphProcessor> graph_processor;
	HashSet<ObjectID> tracked_elements;
	float cell_size = 4.0f;
	uint64_t seed = 0;
	bool auto_materialize = false;
	uint64_t materialize_pass_id = 0;
	String last_error;
	Vector<ObjectID> last_resolved_node_ids;
	AsyncJob *async_job = nullptr;
	WorkerThreadPool::TaskID async_task_id = WorkerThreadPool::INVALID_TASK_ID;

	static void _solve_async_task(void *p_userdata);
	void _add_branch_recursive(Node *p_node);
	void _finish_async_job();
	void _clear_async_job();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_catalog_set(const Ref<WFCCatalogSet> &p_catalog_set);
	Ref<WFCCatalogSet> get_catalog_set() const;
	void set_graph_processor(const Ref<WFCGraphProcessor> &p_graph_processor);
	Ref<WFCGraphProcessor> get_graph_processor() const;

	void set_cell_size(float p_cell_size);
	float get_cell_size() const;

	void set_seed(uint64_t p_seed);
	uint64_t get_seed() const;

	void set_auto_materialize(bool p_auto_materialize);
	bool is_auto_materialize_enabled() const;

	String get_last_error() const;
	TypedArray<WFCElement> get_last_resolved_elements() const;
	bool is_solving() const;

	void reset();
	void cleanup();
	void add_element(WFCElement *p_element);
	void add_branch(Node *p_root);
	int connect_neighbors();
	bool resolve();	bool resolve_sync();
	bool resolve_branch_sync(Node3D *p_root, const Transform3D &p_root_global_transform);	Error resolve_branch_async(Node3D *p_root, const Transform3D &p_root_global_transform);
	Error resolve_async();
	void materialize();

	~WFCSolver();
};