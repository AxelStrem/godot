/**************************************************************************/
/*  test_packed_scene.cpp                                                 */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_packed_scene)

#include "core/config/engine.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/2d/node_2d.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/packed_scene.h"
#include "tests/test_utils.h"

namespace TestPackedScene {

class FilteringNode : public Node {
	GDCLASS(FilteringNode, Node);

	StringName kept_child_name;

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_kept_child_name", "name"), &FilteringNode::set_kept_child_name);
		ClassDB::bind_method(D_METHOD("get_kept_child_name"), &FilteringNode::get_kept_child_name);
		ClassDB::bind_method(D_METHOD("_filter_scene_children", "children"), &FilteringNode::_filter_scene_children);
		ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "kept_child_name"), "set_kept_child_name", "get_kept_child_name");
	}

public:
	static FilteringNode *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(FilteringNode);
			registered = true;
		}
		return memnew(FilteringNode);
	}

	void set_kept_child_name(const StringName &p_name) {
		kept_child_name = p_name;
	}

	StringName get_kept_child_name() const {
		return kept_child_name;
	}

	Array _filter_scene_children(const Array &p_children) const {
		if (kept_child_name == StringName()) {
			return p_children;
		}

		Array filtered_children;
		for (int i = 0; i < p_children.size(); i++) {
			Dictionary child_info = p_children[i];
			if (StringName(child_info["name"]) == kept_child_name) {
				filtered_children.push_back(child_info);
			}
		}
		return filtered_children;
	}
};

class PlanningLeaf : public Node {
	GDCLASS(PlanningLeaf, Node);

	int number = -1;

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_number", "number"), &PlanningLeaf::set_number);
		ClassDB::bind_method(D_METHOD("get_number"), &PlanningLeaf::get_number);
		ADD_PROPERTY(PropertyInfo(Variant::INT, "number"), "set_number", "get_number");
	}

public:
	static PlanningLeaf *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(PlanningLeaf);
			registered = true;
		}
		return memnew(PlanningLeaf);
	}

	void set_number(int p_number) {
		number = p_number;
	}

	int get_number() const {
		return number;
	}
};

class NodeReferenceLeaf : public Node {
	GDCLASS(NodeReferenceLeaf, Node);

	Node *exported_node = nullptr;

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_exported_node", "node"), &NodeReferenceLeaf::set_exported_node);
		ClassDB::bind_method(D_METHOD("get_exported_node"), &NodeReferenceLeaf::get_exported_node);
		ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "exported_node", PROPERTY_HINT_NODE_TYPE, "Node"), "set_exported_node", "get_exported_node");
	}

public:
	static NodeReferenceLeaf *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(NodeReferenceLeaf);
			registered = true;
		}
		return memnew(NodeReferenceLeaf);
	}

	void set_exported_node(Node *p_node) {
		exported_node = p_node;
	}

	Node *get_exported_node() const {
		return exported_node;
	}
};

class ConnectionEmitterNode : public Node {
	GDCLASS(ConnectionEmitterNode, Node);

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("emit_ping"), &ConnectionEmitterNode::emit_ping);
		ADD_SIGNAL(MethodInfo("ping"));
	}

public:
	static ConnectionEmitterNode *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(ConnectionEmitterNode);
			registered = true;
		}
		return memnew(ConnectionEmitterNode);
	}

	void emit_ping() {
		emit_signal("ping");
	}
};

class ConnectionReceiverNode : public Node {
	GDCLASS(ConnectionReceiverNode, Node);

	bool received = false;

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("mark_received"), &ConnectionReceiverNode::mark_received);
	}

public:
	static ConnectionReceiverNode *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(ConnectionReceiverNode);
			registered = true;
		}
		return memnew(ConnectionReceiverNode);
	}

	void mark_received() {
		received = true;
	}

	bool was_received() const {
		return received;
	}
};

class ResourceHolderNode : public Node {
	GDCLASS(ResourceHolderNode, Node);

	Ref<Resource> payload;

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_payload", "payload"), &ResourceHolderNode::set_payload);
		ClassDB::bind_method(D_METHOD("get_payload"), &ResourceHolderNode::get_payload);
		ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "payload", PROPERTY_HINT_RESOURCE_TYPE, "Resource"), "set_payload", "get_payload");
	}

public:
	static ResourceHolderNode *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(ResourceHolderNode);
			registered = true;
		}
		return memnew(ResourceHolderNode);
	}

	void set_payload(const Ref<Resource> &p_payload) {
		payload = p_payload;
	}

	Ref<Resource> get_payload() const {
		return payload;
	}
};

class PlanningNode : public Node {
	GDCLASS(PlanningNode, Node);

	int duplicate_count = 1;

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_duplicate_count", "count"), &PlanningNode::set_duplicate_count);
		ClassDB::bind_method(D_METHOD("get_duplicate_count"), &PlanningNode::get_duplicate_count);
		ClassDB::bind_method(D_METHOD("_customize_scene_instantiation", "plan"), &PlanningNode::_customize_scene_instantiation);
		ADD_PROPERTY(PropertyInfo(Variant::INT, "duplicate_count"), "set_duplicate_count", "get_duplicate_count");
	}

public:
	static PlanningNode *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(PlanningNode);
			registered = true;
		}
		return memnew(PlanningNode);
	}

	void set_duplicate_count(int p_duplicate_count) {
		duplicate_count = p_duplicate_count;
	}

	int get_duplicate_count() const {
		return duplicate_count;
	}

	void _customize_scene_instantiation(const Ref<SceneInstantiationPlanNode> &p_plan) {
		for (int child_idx = 0; child_idx < p_plan->get_child_count(); child_idx++) {
			Ref<SceneInstantiationPlanNode> child_plan = p_plan->get_child(child_idx);
			if (child_plan.is_null()) {
				continue;
			}

			if (child_plan->get_name() == StringName("Drop")) {
				child_plan->prune();
				continue;
			}

			if (child_plan->get_name() == StringName("Template")) {
				child_plan->set_name("Template0");
				child_plan->set_property("number", 0);

				Array duplicates = child_plan->duplicate(MAX(0, duplicate_count - 1));
				for (int duplicate_idx = 0; duplicate_idx < duplicates.size(); duplicate_idx++) {
					Ref<SceneInstantiationPlanNode> duplicate_plan = duplicates[duplicate_idx];
					if (duplicate_plan.is_null()) {
						continue;
					}
					duplicate_plan->set_name(vformat("Template%d", duplicate_idx + 1));
					duplicate_plan->set_property("number", duplicate_idx + 1);
				}
			}
		}
	}
};

class NestedChoosingNode : public Node {
	GDCLASS(NestedChoosingNode, Node);

	StringName kept_child_name;
	int customization_call_count = 0;

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_kept_child_name", "name"), &NestedChoosingNode::set_kept_child_name);
		ClassDB::bind_method(D_METHOD("get_kept_child_name"), &NestedChoosingNode::get_kept_child_name);
		ClassDB::bind_method(D_METHOD("_customize_scene_instantiation", "plan"), &NestedChoosingNode::_customize_scene_instantiation);
		ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "kept_child_name"), "set_kept_child_name", "get_kept_child_name");
	}

public:
	static NestedChoosingNode *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(NestedChoosingNode);
			registered = true;
		}
		return memnew(NestedChoosingNode);
	}

	void set_kept_child_name(const StringName &p_name) {
		kept_child_name = p_name;
	}

	StringName get_kept_child_name() const {
		return kept_child_name;
	}

	int get_customization_call_count() const {
		return customization_call_count;
	}

	void _customize_scene_instantiation(const Ref<SceneInstantiationPlanNode> &p_plan) {
		customization_call_count++;

		Array children = p_plan->get_children();
		for (int child_idx = 0; child_idx < children.size(); child_idx++) {
			Ref<SceneInstantiationPlanNode> child_plan = children[child_idx];
			if (child_plan.is_null()) {
				continue;
			}

			if (child_plan->get_name() != kept_child_name) {
				child_plan->prune();
			}
		}
	}
};

class NestedPropertySelectingNode : public Node {
	GDCLASS(NestedPropertySelectingNode, Node);

	int required_number = -1;
	bool customization_enabled = false;

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_required_number", "number"), &NestedPropertySelectingNode::set_required_number);
		ClassDB::bind_method(D_METHOD("get_required_number"), &NestedPropertySelectingNode::get_required_number);
		ClassDB::bind_method(D_METHOD("set_customization_enabled", "enabled"), &NestedPropertySelectingNode::set_customization_enabled);
		ClassDB::bind_method(D_METHOD("is_customization_enabled"), &NestedPropertySelectingNode::is_customization_enabled);
		ClassDB::bind_method(D_METHOD("_customize_scene_instantiation", "plan"), &NestedPropertySelectingNode::_customize_scene_instantiation);
		ADD_PROPERTY(PropertyInfo(Variant::INT, "required_number"), "set_required_number", "get_required_number");
		ADD_PROPERTY(PropertyInfo(Variant::BOOL, "customization_enabled"), "set_customization_enabled", "is_customization_enabled");
	}

public:
	static NestedPropertySelectingNode *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(NestedPropertySelectingNode);
			registered = true;
		}
		return memnew(NestedPropertySelectingNode);
	}

	void set_required_number(int p_required_number) {
		required_number = p_required_number;
	}

	int get_required_number() const {
		return required_number;
	}

	void set_customization_enabled(bool p_customization_enabled) {
		customization_enabled = p_customization_enabled;
	}

	bool is_customization_enabled() const {
		return customization_enabled;
	}

	void _customize_scene_instantiation(const Ref<SceneInstantiationPlanNode> &p_plan) {
		if (!customization_enabled) {
			return;
		}

		Array children = p_plan->get_children();
		for (int child_idx = 0; child_idx < children.size(); child_idx++) {
			Ref<SceneInstantiationPlanNode> child_plan = children[child_idx];
			if (child_plan.is_null()) {
				continue;
			}

			if (int(child_plan->get_property("number", -1)) != required_number) {
				child_plan->prune();
			}
		}
	}
};

class ExtractingPlanningNode : public Node {
	GDCLASS(ExtractingPlanningNode, Node);

	Ref<PackedScene> extracted_scene;

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("_customize_scene_instantiation", "plan"), &ExtractingPlanningNode::_customize_scene_instantiation);
	}

public:
	static ExtractingPlanningNode *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(ExtractingPlanningNode);
			registered = true;
		}
		return memnew(ExtractingPlanningNode);
	}

	Ref<PackedScene> get_extracted_scene() const {
		return extracted_scene;
	}

	void _customize_scene_instantiation(const Ref<SceneInstantiationPlanNode> &p_plan) {
		for (int child_idx = 0; child_idx < p_plan->get_child_count(); child_idx++) {
			Ref<SceneInstantiationPlanNode> child_plan = p_plan->get_child(child_idx);
			if (child_plan.is_null()) {
				continue;
			}

			if (child_plan->get_name() == StringName("Deferred")) {
				extracted_scene = child_plan->extract_scene();
				child_plan->prune();
			}
		}
	}
};

class NoopPlanningNode : public Node {
	GDCLASS(NoopPlanningNode, Node);

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("_customize_scene_instantiation", "plan"), &NoopPlanningNode::_customize_scene_instantiation);
	}

public:
	static NoopPlanningNode *create_registered() {
		static bool registered = false;
		if (!registered) {
			GDREGISTER_CLASS(NoopPlanningNode);
			registered = true;
		}
		return memnew(NoopPlanningNode);
	}

	void _customize_scene_instantiation(const Ref<SceneInstantiationPlanNode> &p_plan) {
		(void)p_plan;
	}
};

TEST_CASE("[PackedScene] Pack Scene and Retrieve State") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	const Error err = packed_scene.pack(scene);
	CHECK(err == OK);

	// Retrieve the packed state.
	Ref<SceneState> state = packed_scene.get_state();
	CHECK(state.is_valid());
	CHECK(state->get_node_count() == 1);
	CHECK(state->get_node_name(0) == "TestScene");

	memdelete(scene);
}

TEST_CASE("[PackedScene] Packing serializes untweaked base values") {
	Node2D *scene = memnew(Node2D);
	scene->set_name("TestScene");

	const Vector2 base_position(12, 34);
	const Vector2 tweaked_position(56, 78);
	scene->set_position(base_position);

	Ref<Tweak> tweak = scene->create_tweak(scene, SNAME("position"), tweaked_position, Tweak::ACTION_SET, 0);
	REQUIRE(tweak.is_valid());
	CHECK_EQ(scene->get_position(), tweaked_position);
	CHECK_EQ(scene->get_base_value(SNAME("position")), Variant(base_position));

	PackedScene packed_scene;
	CHECK_EQ(packed_scene.pack(scene), OK);

	Node2D *instance = Object::cast_to<Node2D>(packed_scene.instantiate());
	REQUIRE(instance != nullptr);
	CHECK_EQ(instance->get_position(), base_position);

	memdelete(scene);
	memdelete(instance);
}

TEST_CASE("[PackedScene] Saving and reloading keeps untweaked base values") {
	Node2D *scene = memnew(Node2D);
	scene->set_name("TestScene");

	const Vector2 base_position(12, 34);
	const Vector2 tweaked_position(56, 78);
	scene->set_position(base_position);

	Ref<Tweak> tweak = scene->create_tweak(scene, SNAME("position"), tweaked_position, Tweak::ACTION_SET, 0);
	REQUIRE(tweak.is_valid());
	CHECK_EQ(scene->get_position(), tweaked_position);

	Ref<PackedScene> packed_scene;
	packed_scene.instantiate();
	CHECK_EQ(packed_scene->pack(scene), OK);

	const String scene_path = TestUtils::get_temp_path("packed_scene_tweaks_disk_roundtrip.tscn");
	CHECK_EQ(ResourceSaver::save(packed_scene, scene_path), OK);

	Error err = OK;
	Ref<PackedScene> reloaded_scene = ResourceLoader::load(scene_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	CHECK_EQ(err, OK);
	REQUIRE(reloaded_scene.is_valid());

	Node2D *instance = Object::cast_to<Node2D>(reloaded_scene->instantiate());
	REQUIRE(instance != nullptr);
	CHECK_EQ(instance->get_position(), base_position);

	memdelete(scene);
	memdelete(instance);
	DirAccess::remove_file_or_error(scene_path);
}

TEST_CASE("[PackedScene] Saving and reloading keeps untweaked base values for local tweaked children") {
	Node3D *scene = memnew(Node3D);
	scene->set_name("TestScene");

	Node3D *child = memnew(Node3D);
	child->set_name("Top");
	scene->add_child(child);
	child->set_owner(scene);

	const Vector3 base_position(12, 34, 56);
	const Vector3 tweak_offset(3, 5, 7);
	child->set_position(base_position);

	Ref<Tweak> tweak = scene->create_tweak(child, SNAME("position"), tweak_offset, Tweak::ACTION_ADD, 0);
	REQUIRE(tweak.is_valid());
	CHECK_EQ(child->get_position(), base_position + tweak_offset);
	CHECK_EQ(child->get_base_value(SNAME("position")), Variant(base_position));

	Ref<PackedScene> packed_scene;
	packed_scene.instantiate();
	CHECK_EQ(packed_scene->pack(scene), OK);

	const String scene_path = TestUtils::get_temp_path("packed_scene_tweaked_child_disk_roundtrip.tscn");
	CHECK_EQ(ResourceSaver::save(packed_scene, scene_path), OK);

	Error err = OK;
	Ref<PackedScene> reloaded_scene = ResourceLoader::load(scene_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	CHECK_EQ(err, OK);
	REQUIRE(reloaded_scene.is_valid());

	Node3D *instance = Object::cast_to<Node3D>(reloaded_scene->instantiate());
	REQUIRE(instance != nullptr);

	Node3D *reloaded_child = Object::cast_to<Node3D>(instance->get_node_or_null(NodePath("Top")));
	REQUIRE(reloaded_child != nullptr);
	CHECK_EQ(reloaded_child->get_position(), base_position);

	memdelete(scene);
	memdelete(instance);
	DirAccess::remove_file_or_error(scene_path);
}

TEST_CASE("[PackedScene] Signals Preserved when Packing Scene") {
	// Create main scene
	// root
	// `- sub_node (local)
	// `- sub_scene (instance of another scene)
	//    `- sub_scene_node (owned by sub_scene)
	Node *main_scene_root = memnew(Node);
	Node *sub_node = memnew(Node);
	Node *sub_scene_root = memnew(Node);
	Node *sub_scene_node = memnew(Node);

	main_scene_root->add_child(sub_node);
	sub_node->set_owner(main_scene_root);

	sub_scene_root->add_child(sub_scene_node);
	sub_scene_node->set_owner(sub_scene_root);

	main_scene_root->add_child(sub_scene_root);
	sub_scene_root->set_owner(main_scene_root);

	SUBCASE("Signals that should be saved") {
		int main_flags = Object::CONNECT_PERSIST;
		// sub node to a node in main scene
		sub_node->connect("ready", callable_mp(main_scene_root, &Node::is_ready), main_flags);
		// subscene root to a node in main scene
		sub_scene_root->connect("ready", callable_mp(main_scene_root, &Node::is_ready), main_flags);
		//subscene root to subscene root (connected within main scene)
		sub_scene_root->connect("ready", callable_mp(sub_scene_root, &Node::is_ready), main_flags);

		// Pack the scene.
		Ref<PackedScene> packed_scene;
		packed_scene.instantiate();
		const Error err = packed_scene->pack(main_scene_root);
		CHECK(err == OK);

		// Make sure the right connections are in packed scene.
		Ref<SceneState> state = packed_scene->get_state();
		CHECK_EQ(state->get_connection_count(), 3);
	}

	/*
	// FIXME: This subcase requires GH-48064 to be fixed.
	SUBCASE("Signals that should not be saved") {
		int subscene_flags = Object::CONNECT_PERSIST | Object::CONNECT_INHERITED;
		// subscene node to itself
		sub_scene_node->connect("ready", callable_mp(sub_scene_node, &Node::is_ready), subscene_flags);
		// subscene node to subscene root
		sub_scene_node->connect("ready", callable_mp(sub_scene_root, &Node::is_ready), subscene_flags);
		//subscene root to subscene root (connected within sub scene)
		sub_scene_root->connect("ready", callable_mp(sub_scene_root, &Node::is_ready), subscene_flags);

		// Pack the scene.
		Ref<PackedScene> packed_scene;
		packed_scene.instantiate();
		const Error err = packed_scene->pack(main_scene_root);
		CHECK(err == OK);

		// Make sure the right connections are in packed scene.
		Ref<SceneState> state = packed_scene->get_state();
		CHECK_EQ(state->get_connection_count(), 0);
	}
	*/

	memdelete(main_scene_root);
}

TEST_CASE("[PackedScene] Pack scene preserves host-owned children under exposed instance descendants") {
	Node *sub_scene_root = memnew(Node);
	sub_scene_root->set_name("SubSceneRoot");

	Node *wrapper = memnew(Node);
	wrapper->set_name("Wrapper");
	sub_scene_root->add_child(wrapper);
	wrapper->set_owner(sub_scene_root);

	Node *exposed = memnew(Node);
	exposed->set_name("Exposed");
	exposed->set_exposed_to_owner(true);
	wrapper->add_child(exposed);
	exposed->set_owner(sub_scene_root);

	Ref<PackedScene> sub_scene;
	sub_scene.instantiate();
	CHECK_EQ(sub_scene->pack(sub_scene_root), OK);
	Ref<SceneState> sub_scene_state = sub_scene->get_state();
	REQUIRE(sub_scene_state.is_valid());
	CHECK_EQ(sub_scene_state->get_exposed_children().size(), 1);
	if (sub_scene_state->get_exposed_children().size() == 1) {
		CHECK_EQ(String(sub_scene_state->get_exposed_children()[0]), String("Wrapper/Exposed"));
	}

	const String sub_scene_path = TestUtils::get_temp_path("packed_scene_exposed_child_host_addition.tscn");
	CHECK_EQ(ResourceSaver::save(sub_scene, sub_scene_path), OK);

	Ref<PackedScene> loaded_sub_scene = ResourceLoader::load(sub_scene_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(loaded_sub_scene.is_valid());
	Ref<SceneState> loaded_sub_scene_state = loaded_sub_scene->get_state();
	REQUIRE(loaded_sub_scene_state.is_valid());
	CHECK_EQ(loaded_sub_scene_state->get_exposed_children().size(), 1);
	if (loaded_sub_scene_state->get_exposed_children().size() == 1) {
		CHECK_EQ(String(loaded_sub_scene_state->get_exposed_children()[0]), String("Wrapper/Exposed"));
	}

	Node *main_scene_root = memnew(Node);
	main_scene_root->set_name("MainSceneRoot");

	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	Node *instanced = loaded_sub_scene->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(instanced != nullptr);
	instanced->set_name("Instanced");
	main_scene_root->add_child(instanced);
	instanced->set_owner(main_scene_root);

	Node *exposed_in_owner = instanced->get_node_or_null(NodePath("Wrapper/Exposed"));
	REQUIRE(exposed_in_owner != nullptr);
	CHECK(exposed_in_owner->is_exposed_to_owner());
	CHECK(Node::_has_exposed_descendant(instanced));

	Node *added = memnew(Node);
	added->set_name("Added");
	exposed_in_owner->add_child(added);
	added->set_owner(main_scene_root);
	added->connect("ready", Callable(main_scene_root, SNAME("queue_free")), Object::CONNECT_PERSIST);

	Node *helper = memnew(Node);
	helper->set_name("Helper");
	helper->add_to_group("EditorHelper", true);
	exposed_in_owner->add_child(helper);

	Ref<PackedScene> main_scene;
	main_scene.instantiate();
	CHECK_EQ(main_scene->pack(main_scene_root), OK);

	Ref<SceneState> main_state = main_scene->get_state();
	REQUIRE(main_state.is_valid());
	CHECK_EQ(main_state->get_node_count(), 3);
	CHECK_EQ(main_state->get_connection_count(), 1);

	Node *instantiated_main = main_scene->instantiate();
	REQUIRE(instantiated_main != nullptr);

	Node *instantiated_added = instantiated_main->get_node_or_null(NodePath("Instanced/Wrapper/Exposed/Added"));
	CHECK(instantiated_added != nullptr);
	if (instantiated_added != nullptr) {
		CHECK_EQ(instantiated_added->get_owner(), instantiated_main);
	}

	Node *instantiated_helper = instantiated_main->get_node_or_null(NodePath("Instanced/Wrapper/Exposed/Helper"));
	CHECK(instantiated_helper == nullptr);

	memdelete(sub_scene_root);
	memdelete(main_scene_root);
	memdelete(instantiated_main);
	DirAccess::remove_file_or_error(sub_scene_path);
}

TEST_CASE("[PackedScene] Owner-scoped exposed descendants do not leak across nested edited instances") {
	Node *scene_c_root = memnew(Node);
	scene_c_root->set_name("SceneC");

	Node *exposed_c = memnew(Node);
	exposed_c->set_name("ExposedC");
	exposed_c->set_exposed_to_owner(true);
	scene_c_root->add_child(exposed_c);
	exposed_c->set_owner(scene_c_root);

	Ref<PackedScene> scene_c;
	scene_c.instantiate();
	CHECK_EQ(scene_c->pack(scene_c_root), OK);

	const String scene_c_path = TestUtils::get_temp_path("packed_scene_nested_exposed_scene_c.tscn");
	CHECK_EQ(ResourceSaver::save(scene_c, scene_c_path), OK);

	Ref<PackedScene> loaded_scene_c = ResourceLoader::load(scene_c_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(loaded_scene_c.is_valid());

	Node *scene_b_root = memnew(Node);
	scene_b_root->set_name("SceneB");

	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	Node *instanced_c = loaded_scene_c->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(instanced_c != nullptr);
	instanced_c->set_name("InstancedC");
	scene_b_root->add_child(instanced_c);
	instanced_c->set_owner(scene_b_root);

	CHECK(Node::_has_exposed_descendant(instanced_c));
	CHECK(Node::_has_exposed_descendant_for_owner(instanced_c, scene_b_root));

	Ref<PackedScene> scene_b;
	scene_b.instantiate();
	CHECK_EQ(scene_b->pack(scene_b_root), OK);

	const String scene_b_path = TestUtils::get_temp_path("packed_scene_nested_exposed_scene_b.tscn");
	CHECK_EQ(ResourceSaver::save(scene_b, scene_b_path), OK);

	Ref<PackedScene> loaded_scene_b = ResourceLoader::load(scene_b_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(loaded_scene_b.is_valid());

	Node *scene_a_root = memnew(Node);
	scene_a_root->set_name("SceneA");

	Engine::get_singleton()->set_editor_hint(true);
	Node *instanced_b = loaded_scene_b->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(instanced_b != nullptr);
	instanced_b->set_name("InstancedB");
	scene_a_root->add_child(instanced_b);
	instanced_b->set_owner(scene_a_root);

	CHECK(Node::_has_exposed_descendant(instanced_b));
	CHECK_FALSE(Node::_has_exposed_descendant_for_owner(instanced_b, scene_a_root));

	memdelete(scene_c_root);
	memdelete(scene_b_root);
	memdelete(scene_a_root);
	DirAccess::remove_file_or_error(scene_b_path);
	DirAccess::remove_file_or_error(scene_c_path);
}

TEST_CASE("[PackedScene] Nested exposed nodes can be re-exposed by the host scene") {
	Node *scene_c_root = memnew(Node);
	scene_c_root->set_name("SceneC");

	Node *exposed_d = memnew(Node);
	exposed_d->set_name("D");
	exposed_d->set_exposed_to_owner(true);
	scene_c_root->add_child(exposed_d);
	exposed_d->set_owner(scene_c_root);

	Ref<PackedScene> scene_c;
	scene_c.instantiate();
	CHECK_EQ(scene_c->pack(scene_c_root), OK);

	const String scene_c_path = TestUtils::get_temp_path("packed_scene_nested_reexpose_scene_c.tscn");
	CHECK_EQ(ResourceSaver::save(scene_c, scene_c_path), OK);

	Ref<PackedScene> loaded_scene_c = ResourceLoader::load(scene_c_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(loaded_scene_c.is_valid());

	Node *scene_b_root = memnew(Node);
	scene_b_root->set_name("SceneB");

	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	Node *instanced_c = loaded_scene_c->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(instanced_c != nullptr);
	instanced_c->set_name("InstancedC");
	scene_b_root->add_child(instanced_c);
	instanced_c->set_owner(scene_b_root);

	Node *exposed_in_b = instanced_c->get_node_or_null(NodePath("D"));
	REQUIRE(exposed_in_b != nullptr);
	CHECK(Node::_is_exposed_to_scene_owner(exposed_in_b, scene_b_root));
	CHECK_FALSE(scene_b_root->is_exposed_node_to_owner(exposed_in_b));

	scene_b_root->set_exposed_node_to_owner(exposed_in_b, true);
	CHECK(scene_b_root->is_exposed_node_to_owner(exposed_in_b));

	Ref<PackedScene> scene_b;
	scene_b.instantiate();
	CHECK_EQ(scene_b->pack(scene_b_root), OK);
	Ref<SceneState> scene_b_state = scene_b->get_state();
	REQUIRE(scene_b_state.is_valid());
	CHECK_EQ(scene_b_state->get_exposed_children().size(), 1);
	if (scene_b_state->get_exposed_children().size() == 1) {
		CHECK_EQ(String(scene_b_state->get_exposed_children()[0]), String("InstancedC/D"));
	}

	const String scene_b_path = TestUtils::get_temp_path("packed_scene_nested_reexpose_scene_b.tscn");
	CHECK_EQ(ResourceSaver::save(scene_b, scene_b_path), OK);

	Ref<PackedScene> loaded_scene_b = ResourceLoader::load(scene_b_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(loaded_scene_b.is_valid());

	Node *scene_a_root = memnew(Node);
	scene_a_root->set_name("SceneA");

	Engine::get_singleton()->set_editor_hint(true);
	Node *instanced_b = loaded_scene_b->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(instanced_b != nullptr);
	instanced_b->set_name("InstancedB");
	scene_a_root->add_child(instanced_b);
	instanced_b->set_owner(scene_a_root);

	Node *exposed_in_a = instanced_b->get_node_or_null(NodePath("InstancedC/D"));
	REQUIRE(exposed_in_a != nullptr);
	CHECK(instanced_b->is_exposed_node_to_owner(exposed_in_a));
	CHECK(Node::_is_exposed_to_scene_owner(exposed_in_a, scene_a_root));

	memdelete(scene_c_root);
	memdelete(scene_b_root);
	memdelete(scene_a_root);
	DirAccess::remove_file_or_error(scene_b_path);
	DirAccess::remove_file_or_error(scene_c_path);
}

TEST_CASE("[PackedScene] Clear Packed Scene") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Clear the packed scene.
	packed_scene.clear();

	// Check if it has been cleared.
	Ref<SceneState> state = packed_scene.get_state();
	CHECK_FALSE(state->get_node_count() == 1);

	memdelete(scene);
}

TEST_CASE("[PackedScene] Can Instantiate Packed Scene") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Check if the packed scene can be instantiated.
	const bool can_instantiate = packed_scene.can_instantiate();
	CHECK(can_instantiate == true);

	memdelete(scene);
}

TEST_CASE("[PackedScene] Instantiate Packed Scene") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Instantiate the packed scene.
	Node *instance = packed_scene.instantiate();
	CHECK(instance != nullptr);
	CHECK(instance->get_name() == "TestScene");

	memdelete(scene);
	memdelete(instance);
}

TEST_CASE("[PackedScene] Instantiate Packed Scene With Children") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Add persisting child nodes to the scene.
	Node *child1 = memnew(Node);
	child1->set_name("Child1");
	scene->add_child(child1);
	child1->set_owner(scene);

	Node *child2 = memnew(Node);
	child2->set_name("Child2");
	scene->add_child(child2);
	child2->set_owner(scene);

	// Add non persisting child node to the scene.
	Node *child3 = memnew(Node);
	child3->set_name("Child3");
	scene->add_child(child3);

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Instantiate the packed scene.
	Node *instance = packed_scene.instantiate();
	CHECK(instance != nullptr);
	CHECK(instance->get_name() == "TestScene");

	// Validate the child nodes of the instantiated scene.
	CHECK(instance->get_child_count() == 2);
	CHECK(instance->get_child(0)->get_name() == "Child1");
	CHECK(instance->get_child(1)->get_name() == "Child2");
	CHECK(instance->get_child(0)->get_owner() == instance);
	CHECK(instance->get_child(1)->get_owner() == instance);

	memdelete(scene);
	memdelete(instance);
}

TEST_CASE("[PackedScene] Runtime plan keeps instanced child roots owned") {
	Node *child_root = memnew(Node);
	child_root->set_name("ChildRoot");

	Node *child_leaf = memnew(Node);
	child_leaf->set_name("Leaf");
	child_root->add_child(child_leaf);
	child_leaf->set_owner(child_root);

	Ref<PackedScene> child_scene;
	child_scene.instantiate();
	CHECK_EQ(child_scene->pack(child_root), OK);

	const String child_path = TestUtils::get_temp_path("runtime_plan_instanced_child_owner.tscn");
	CHECK_EQ(ResourceSaver::save(child_scene, child_path), OK);

	Error err = OK;
	Ref<PackedScene> child_scene_loaded = ResourceLoader::load(child_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(child_scene_loaded.is_valid());

	NestedChoosingNode *main_root = NestedChoosingNode::create_registered();
	main_root->set_name("MainRoot");
	main_root->set_kept_child_name("InstancedChild");

	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	Node *instanced_child = child_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(instanced_child != nullptr);
	instanced_child->set_name("InstancedChild");
	main_root->add_child(instanced_child);
	instanced_child->set_owner(main_root);

	Engine::get_singleton()->set_editor_hint(true);
	PackedScene main_scene;
	CHECK_EQ(main_scene.pack(main_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = main_scene.instantiate();
	REQUIRE(instance != nullptr);

	Node *found_child = instance->find_child("InstancedChild", true, true);
	REQUIRE(found_child != nullptr);
	CHECK_EQ(found_child->get_owner(), instance);

	memdelete(child_root);
	memdelete(main_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(child_path);
}

TEST_CASE("[PackedScene] Runtime plan expands inherited roots before customization") {
	PlanningNode *base_root = PlanningNode::create_registered();
	base_root->set_name("MainRoot");

	Node *camera = memnew(Node);
	camera->set_name("Camera");
	base_root->add_child(camera);
	camera->set_owner(base_root);

	Node *hud = memnew(Node);
	hud->set_name("HUD");
	camera->add_child(hud);
	hud->set_owner(base_root);

	Ref<PackedScene> base_scene;
	base_scene.instantiate();
	CHECK_EQ(base_scene->pack(base_root), OK);

	const String base_path = TestUtils::get_temp_path("runtime_plan_inherited_root_base.tscn");
	CHECK_EQ(ResourceSaver::save(base_scene, base_path), OK);

	const String inherited_path = TestUtils::get_temp_path("runtime_plan_inherited_root_scene.tscn");
	Ref<FileAccess> inherited_file = FileAccess::open(inherited_path, FileAccess::WRITE);
	REQUIRE(inherited_file.is_valid());
	inherited_file->store_string(vformat("[gd_scene load_steps=2 format=3]\n\n[ext_resource type=\"PackedScene\" path=\"%s\" id=\"1_base\"]\n\n[node name=\"MainRoot\" instance=ExtResource(\"1_base\")]\n", base_path.replace("\\", "/")));
	inherited_file.unref();

	Error err = OK;
	Ref<PackedScene> inherited_scene = ResourceLoader::load(inherited_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(inherited_scene.is_valid());

	Node *instance = inherited_scene->instantiate();
	REQUIRE(instance != nullptr);

	Node *instanced_camera = instance->get_node_or_null(NodePath("Camera"));
	Node *instanced_hud = instance->get_node_or_null(NodePath("Camera/HUD"));
	REQUIRE(instanced_camera != nullptr);
	REQUIRE(instanced_hud != nullptr);
	CHECK_EQ(instanced_camera->get_owner(), instance);
	CHECK_EQ(instanced_hud->get_owner(), instance);

	memdelete(base_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(base_path);
	DirAccess::remove_file_or_error(inherited_path);
}

TEST_CASE("[PackedScene] Runtime plan merges nested wrapper scene root overrides") {
	NestedChoosingNode *base_root = NestedChoosingNode::create_registered();
	base_root->set_name("WrappedRoot");
	base_root->set_kept_child_name("TemplateKeep");

	PlanningLeaf *base_keep = PlanningLeaf::create_registered();
	base_keep->set_name("TemplateKeep");
	base_keep->set_number(10);
	base_root->add_child(base_keep);
	base_keep->set_owner(base_root);

	PlanningLeaf *base_drop = PlanningLeaf::create_registered();
	base_drop->set_name("TemplateDrop");
	base_drop->set_number(20);
	base_root->add_child(base_drop);
	base_drop->set_owner(base_root);

	Ref<PackedScene> base_scene;
	base_scene.instantiate();
	CHECK_EQ(base_scene->pack(base_root), OK);

	const String base_path = TestUtils::get_temp_path("runtime_plan_nested_wrapper_base.tscn");
	CHECK_EQ(ResourceSaver::save(base_scene, base_path), OK);

	Error err = OK;
	Ref<PackedScene> base_scene_loaded = ResourceLoader::load(base_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(base_scene_loaded.is_valid());

	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	NestedChoosingNode *wrapper_root = Object::cast_to<NestedChoosingNode>(base_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN));
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(wrapper_root != nullptr);
	wrapper_root->set_name("Wrapped");
	wrapper_root->set_kept_child_name("OwnerAdded");

	PlanningLeaf *owner_added = PlanningLeaf::create_registered();
	owner_added->set_name("OwnerAdded");
	owner_added->set_number(42);
	wrapper_root->add_child(owner_added);
	owner_added->set_owner(wrapper_root);

	Ref<PackedScene> wrapper_scene;
	wrapper_scene.instantiate();
	Engine::get_singleton()->set_editor_hint(true);
	CHECK_EQ(wrapper_scene->pack(wrapper_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	const String wrapper_path = TestUtils::get_temp_path("runtime_plan_nested_wrapper_scene.tscn");
	CHECK_EQ(ResourceSaver::save(wrapper_scene, wrapper_path), OK);

	Ref<PackedScene> wrapper_scene_loaded = ResourceLoader::load(wrapper_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(wrapper_scene_loaded.is_valid());

	Node *outer_root = memnew(Node);
	outer_root->set_name("Outer");

	Engine::get_singleton()->set_editor_hint(true);
	Node *nested_wrapper = wrapper_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(nested_wrapper != nullptr);
	outer_root->add_child(nested_wrapper);
	nested_wrapper->set_owner(outer_root);

	PackedScene outer_scene;
	Engine::get_singleton()->set_editor_hint(true);
	CHECK_EQ(outer_scene.pack(outer_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = outer_scene.instantiate();
	REQUIRE(instance != nullptr);

	NestedChoosingNode *instanced_wrapper = Object::cast_to<NestedChoosingNode>(instance->get_node_or_null(NodePath("Wrapped")));
	REQUIRE(instanced_wrapper != nullptr);
	CHECK_EQ(instanced_wrapper->get_child_count(), 1);

	PlanningLeaf *surviving_child = Object::cast_to<PlanningLeaf>(instanced_wrapper->get_child(0));
	REQUIRE(surviving_child != nullptr);
	CHECK_EQ(surviving_child->get_name(), StringName("OwnerAdded"));
	CHECK_EQ(surviving_child->get_number(), 42);

	memdelete(base_root);
	memdelete(wrapper_root);
	memdelete(outer_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(base_path);
	DirAccess::remove_file_or_error(wrapper_path);
}

TEST_CASE("[PackedScene] Runtime plan expands nested instanced wrapper roots") {
	Node *base_root = memnew(Node);
	base_root->set_name("WrappedRoot");

	PlanningLeaf *base_child = PlanningLeaf::create_registered();
	base_child->set_name("Inner");
	base_child->set_number(10);
	base_root->add_child(base_child);
	base_child->set_owner(base_root);

	Ref<PackedScene> base_scene;
	base_scene.instantiate();
	CHECK_EQ(base_scene->pack(base_root), OK);

	const String base_path = TestUtils::get_temp_path("runtime_plan_nested_instanced_wrapper_base.tscn");
	CHECK_EQ(ResourceSaver::save(base_scene, base_path), OK);

	const String wrapper_path = TestUtils::get_temp_path("runtime_plan_nested_instanced_wrapper_scene.tscn");
	Ref<FileAccess> wrapper_file = FileAccess::open(wrapper_path, FileAccess::WRITE);
	REQUIRE(wrapper_file.is_valid());
	wrapper_file->store_string(vformat("[gd_scene load_steps=2 format=3]\n\n[ext_resource type=\"PackedScene\" path=\"%s\" id=\"1_base\"]\n\n[node name=\"Wrapped\" instance=ExtResource(\"1_base\")]\n\n[node name=\"Inner\" parent=\".\"]\nnumber = 42\n", base_path.replace("\\", "/")));
	wrapper_file.unref();

	Error err = OK;
	Ref<PackedScene> wrapper_scene = ResourceLoader::load(wrapper_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(wrapper_scene.is_valid());

	NestedChoosingNode *outer_root = NestedChoosingNode::create_registered();
	outer_root->set_name("Outer");
	outer_root->set_kept_child_name("Wrapped");

	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	Node *nested_wrapper = wrapper_scene->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(nested_wrapper != nullptr);
	outer_root->add_child(nested_wrapper);
	nested_wrapper->set_owner(outer_root);

	PackedScene outer_scene;
	Engine::get_singleton()->set_editor_hint(true);
	CHECK_EQ(outer_scene.pack(outer_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = outer_scene.instantiate();
	REQUIRE(instance != nullptr);

	Node *wrapped = instance->get_node_or_null(NodePath("Wrapped"));
	REQUIRE(wrapped != nullptr);

	PlanningLeaf *instanced_inner = Object::cast_to<PlanningLeaf>(wrapped->get_node_or_null(NodePath("Inner")));
	REQUIRE(instanced_inner != nullptr);
	CHECK_EQ(instanced_inner->get_number(), 42);
	CHECK_EQ(instanced_inner->get_owner(), wrapped);

	memdelete(base_root);
	memdelete(outer_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(base_path);
	DirAccess::remove_file_or_error(wrapper_path);
}

TEST_CASE("[PackedScene] Runtime plan applies descendant overrides across inherited scenes") {
	PlanningNode *base_root = PlanningNode::create_registered();
	base_root->set_name("Root");

	Node *base_branch = memnew(Node);
	base_branch->set_name("Base");
	base_root->add_child(base_branch);
	base_branch->set_owner(base_root);

	PlanningLeaf *base_leaf = PlanningLeaf::create_registered();
	base_leaf->set_name("Leaf");
	base_leaf->set_number(10);
	base_branch->add_child(base_leaf);
	base_leaf->set_owner(base_root);

	Ref<PackedScene> base_scene;
	base_scene.instantiate();
	CHECK_EQ(base_scene->pack(base_root), OK);

	const String base_path = TestUtils::get_temp_path("runtime_plan_inherited_descendant_override_base.tscn");
	CHECK_EQ(ResourceSaver::save(base_scene, base_path), OK);

	Error err = OK;
	Ref<PackedScene> base_scene_loaded = ResourceLoader::load(base_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(base_scene_loaded.is_valid());

	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	Node *mid_root = base_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(mid_root != nullptr);

	PlanningLeaf *mid_leaf = Object::cast_to<PlanningLeaf>(mid_root->get_node_or_null(NodePath("Base/Leaf")));
	REQUIRE(mid_leaf != nullptr);
	mid_leaf->set_number(20);

	Ref<PackedScene> mid_scene;
	mid_scene.instantiate();
	Engine::get_singleton()->set_editor_hint(true);
	CHECK_EQ(mid_scene->pack(mid_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	const String mid_path = TestUtils::get_temp_path("runtime_plan_inherited_descendant_override_mid.tscn");
	CHECK_EQ(ResourceSaver::save(mid_scene, mid_path), OK);

	Ref<PackedScene> mid_scene_loaded = ResourceLoader::load(mid_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(mid_scene_loaded.is_valid());

	Engine::get_singleton()->set_editor_hint(true);
	Node *child_root = mid_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(child_root != nullptr);

	PlanningLeaf *child_leaf = Object::cast_to<PlanningLeaf>(child_root->get_node_or_null(NodePath("Base/Leaf")));
	REQUIRE(child_leaf != nullptr);
	child_leaf->set_number(30);

	Ref<PackedScene> child_scene;
	child_scene.instantiate();
	Engine::get_singleton()->set_editor_hint(true);
	CHECK_EQ(child_scene->pack(child_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	const String child_path = TestUtils::get_temp_path("runtime_plan_inherited_descendant_override_child.tscn");
	CHECK_EQ(ResourceSaver::save(child_scene, child_path), OK);

	Ref<PackedScene> child_scene_loaded = ResourceLoader::load(child_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(child_scene_loaded.is_valid());

	Node *instance = child_scene_loaded->instantiate();
	REQUIRE(instance != nullptr);

	PlanningLeaf *instanced_leaf = Object::cast_to<PlanningLeaf>(instance->get_node_or_null(NodePath("Base/Leaf")));
	REQUIRE(instanced_leaf != nullptr);
	CHECK_EQ(instanced_leaf->get_number(), 30);

	memdelete(base_root);
	memdelete(mid_root);
	memdelete(child_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(base_path);
	DirAccess::remove_file_or_error(mid_path);
	DirAccess::remove_file_or_error(child_path);
}

TEST_CASE("[PackedScene] Runtime plan applies raw descendant overrides across inherited scenes") {
	PlanningNode *base_root = PlanningNode::create_registered();
	base_root->set_name("Root");

	Node *base_branch = memnew(Node);
	base_branch->set_name("Base");
	base_branch->set_unique_scene_id(101);
	base_root->add_child(base_branch);
	base_branch->set_owner(base_root);

	PlanningLeaf *base_leaf = PlanningLeaf::create_registered();
	base_leaf->set_name("Leaf");
	base_leaf->set_number(10);
	base_leaf->set_unique_scene_id(202);
	base_branch->add_child(base_leaf);
	base_leaf->set_owner(base_root);

	Ref<PackedScene> base_scene;
	base_scene.instantiate();
	CHECK_EQ(base_scene->pack(base_root), OK);

	const String base_path = TestUtils::get_temp_path("runtime_plan_raw_inherited_descendant_override_base.tscn");
	CHECK_EQ(ResourceSaver::save(base_scene, base_path), OK);

	const String mid_path = TestUtils::get_temp_path("runtime_plan_raw_inherited_descendant_override_mid.tscn");
	Ref<FileAccess> mid_file = FileAccess::open(mid_path, FileAccess::WRITE);
	REQUIRE(mid_file.is_valid());
	mid_file->store_string(vformat("[gd_scene load_steps=2 format=3]\n\n[ext_resource type=\"PackedScene\" path=\"%s\" id=\"1_base\"]\n\n[node name=\"Root\" instance=ExtResource(\"1_base\")]\n\n[node name=\"Leaf\" parent=\"Base\" parent_id_path=PackedInt32Array(101)]\nnumber = 20\n", base_path.replace("\\", "/")));
	mid_file.unref();

	const String child_path = TestUtils::get_temp_path("runtime_plan_raw_inherited_descendant_override_child.tscn");
	Ref<FileAccess> child_file = FileAccess::open(child_path, FileAccess::WRITE);
	REQUIRE(child_file.is_valid());
	child_file->store_string(vformat("[gd_scene load_steps=2 format=3]\n\n[ext_resource type=\"PackedScene\" path=\"%s\" id=\"1_mid\"]\n\n[node name=\"Root\" instance=ExtResource(\"1_mid\")]\n\n[node name=\"Leaf\" parent=\"Base\" parent_id_path=PackedInt32Array(101)]\nnumber = 30\n", mid_path.replace("\\", "/")));
	child_file.unref();

	Error err = OK;
	Ref<PackedScene> child_scene = ResourceLoader::load(child_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(child_scene.is_valid());

	Node *instance = child_scene->instantiate();
	REQUIRE(instance != nullptr);

	PlanningLeaf *instanced_leaf = Object::cast_to<PlanningLeaf>(instance->get_node_or_null(NodePath("Base/Leaf")));
	REQUIRE(instanced_leaf != nullptr);
	CHECK_EQ(instanced_leaf->get_number(), 30);

	memdelete(base_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(base_path);
	DirAccess::remove_file_or_error(mid_path);
	DirAccess::remove_file_or_error(child_path);
}

TEST_CASE("[PackedScene] Runtime plan tolerates unavailable node classes") {
	const String scene_path = TestUtils::get_temp_path("runtime_plan_unknown_node_type.tscn");
	Ref<FileAccess> scene_file = FileAccess::open(scene_path, FileAccess::WRITE);
	REQUIRE(scene_file.is_valid());
	scene_file->store_string("[gd_scene format=3]\n\n[node name=\"Root\" type=\"PlanningNode\"]\n\n[node name=\"Broken\" type=\"MissingFancyNode\" parent=\".\"]\n");
	scene_file.unref();

	Error err = OK;
	Ref<PackedScene> scene = ResourceLoader::load(scene_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(scene.is_valid());

	Node *instance = scene->instantiate();
	REQUIRE(instance != nullptr);

	Node *broken = instance->get_node_or_null(NodePath("Broken"));
	REQUIRE(broken != nullptr);
	CHECK_EQ(broken->get_name(), StringName("Broken"));

	memdelete(instance);
	DirAccess::remove_file_or_error(scene_path);
}

TEST_CASE("[PackedScene] Nested child filtering uses canonical paths") {
	Node *scene = memnew(Node);
	scene->set_name("Root");

	Node *branch_a = memnew(Node);
	branch_a->set_name("BranchA");
	scene->add_child(branch_a);
	branch_a->set_owner(scene);

	FilteringNode *filter_a = FilteringNode::create_registered();
	filter_a->set_name("Inner");
	filter_a->set_kept_child_name("Keep");
	branch_a->add_child(filter_a);
	filter_a->set_owner(scene);

	Node *keep_a = memnew(Node);
	keep_a->set_name("Keep");
	filter_a->add_child(keep_a);
	keep_a->set_owner(scene);

	Node *drop_a = memnew(Node);
	drop_a->set_name("Drop");
	filter_a->add_child(drop_a);
	drop_a->set_owner(scene);

	Node *branch_b = memnew(Node);
	branch_b->set_name("BranchB");
	scene->add_child(branch_b);
	branch_b->set_owner(scene);

	FilteringNode *filter_b = FilteringNode::create_registered();
	filter_b->set_name("Inner");
	filter_b->set_kept_child_name("Drop");
	branch_b->add_child(filter_b);
	filter_b->set_owner(scene);

	Node *keep_b = memnew(Node);
	keep_b->set_name("Keep");
	filter_b->add_child(keep_b);
	keep_b->set_owner(scene);

	Node *drop_b = memnew(Node);
	drop_b->set_name("Drop");
	filter_b->add_child(drop_b);
	drop_b->set_owner(scene);

	PackedScene packed_scene;
	CHECK_EQ(packed_scene.pack(scene), OK);

	Node *instance = packed_scene.instantiate();
	REQUIRE(instance != nullptr);

	Node *inner_a = instance->get_node_or_null(NodePath("BranchA/Inner"));
	Node *inner_b = instance->get_node_or_null(NodePath("BranchB/Inner"));
	REQUIRE(inner_a != nullptr);
	REQUIRE(inner_b != nullptr);

	CHECK_EQ(inner_a->get_child_count(), 1);
	CHECK_EQ(inner_b->get_child_count(), 1);
	CHECK_EQ(inner_a->get_child(0)->get_name(), StringName("Keep"));
	CHECK_EQ(inner_b->get_child(0)->get_name(), StringName("Drop"));

	memdelete(scene);
	memdelete(instance);
}

TEST_CASE("[PackedScene] Runtime plan customization prunes and duplicates children") {
	PlanningNode *scene = PlanningNode::create_registered();
	scene->set_name("Root");
	scene->set_duplicate_count(3);

	PlanningLeaf *template_leaf = PlanningLeaf::create_registered();
	template_leaf->set_name("Template");
	template_leaf->set_number(99);
	scene->add_child(template_leaf);
	template_leaf->set_owner(scene);

	PlanningLeaf *drop_leaf = PlanningLeaf::create_registered();
	drop_leaf->set_name("Drop");
	drop_leaf->set_number(777);
	scene->add_child(drop_leaf);
	drop_leaf->set_owner(scene);

	PackedScene packed_scene;
	CHECK_EQ(packed_scene.pack(scene), OK);

	Node *instance = packed_scene.instantiate();
	REQUIRE(instance != nullptr);

	CHECK_EQ(instance->get_child_count(), 3);

	PlanningLeaf *leaf_0 = Object::cast_to<PlanningLeaf>(instance->get_child(0));
	PlanningLeaf *leaf_1 = Object::cast_to<PlanningLeaf>(instance->get_child(1));
	PlanningLeaf *leaf_2 = Object::cast_to<PlanningLeaf>(instance->get_child(2));
	REQUIRE(leaf_0 != nullptr);
	REQUIRE(leaf_1 != nullptr);
	REQUIRE(leaf_2 != nullptr);

	CHECK_EQ(leaf_0->get_name(), StringName("Template0"));
	CHECK_EQ(leaf_1->get_name(), StringName("Template1"));
	CHECK_EQ(leaf_2->get_name(), StringName("Template2"));
	CHECK_EQ(leaf_0->get_number(), 0);
	CHECK_EQ(leaf_1->get_number(), 1);
	CHECK_EQ(leaf_2->get_number(), 2);

	memdelete(scene);
	memdelete(instance);
}

TEST_CASE("[PackedScene] Runtime plan customization resolves deferred node properties") {
	PlanningNode *scene = PlanningNode::create_registered();
	scene->set_name("Scene");

	Node *target = memnew(Node);
	target->set_name("Target");
	scene->add_child(target);
	target->set_owner(scene);

	NodeReferenceLeaf *reference_leaf = NodeReferenceLeaf::create_registered();
	reference_leaf->set_name("Reference");
	reference_leaf->set_exported_node(target);
	scene->add_child(reference_leaf);
	reference_leaf->set_owner(scene);

	PackedScene packed_scene;
	CHECK_EQ(packed_scene.pack(scene), OK);

	Node *instance = packed_scene.instantiate();
	REQUIRE(instance != nullptr);

	Node *instanced_target = instance->get_node_or_null(NodePath("Target"));
	NodeReferenceLeaf *instanced_reference = Object::cast_to<NodeReferenceLeaf>(instance->get_node_or_null(NodePath("Reference")));
	REQUIRE(instanced_target != nullptr);
	REQUIRE(instanced_reference != nullptr);
	CHECK_EQ(instanced_reference->get_exported_node(), instanced_target);

	memdelete(scene);
	memdelete(instance);
}

TEST_CASE("[PackedScene] Runtime plan customization preserves persistent connections") {
	PlanningNode *scene = PlanningNode::create_registered();
	scene->set_name("Scene");

	ConnectionEmitterNode *emitter = ConnectionEmitterNode::create_registered();
	emitter->set_name("Emitter");
	scene->add_child(emitter);
	emitter->set_owner(scene);

	ConnectionReceiverNode *receiver = ConnectionReceiverNode::create_registered();
	receiver->set_name("Receiver");
	scene->add_child(receiver);
	receiver->set_owner(scene);

	CHECK_EQ(emitter->connect("ping", Callable(receiver, "mark_received"), Object::CONNECT_PERSIST), OK);

	PackedScene packed_scene;
	CHECK_EQ(packed_scene.pack(scene), OK);

	Node *instance = packed_scene.instantiate();
	REQUIRE(instance != nullptr);

	ConnectionEmitterNode *instanced_emitter = Object::cast_to<ConnectionEmitterNode>(instance->get_node_or_null(NodePath("Emitter")));
	ConnectionReceiverNode *instanced_receiver = Object::cast_to<ConnectionReceiverNode>(instance->get_node_or_null(NodePath("Receiver")));
	REQUIRE(instanced_emitter != nullptr);
	REQUIRE(instanced_receiver != nullptr);
	CHECK(instanced_emitter->is_connected("ping", Callable(instanced_receiver, "mark_received")));

	instanced_emitter->emit_ping();
	CHECK(instanced_receiver->was_received());

	memdelete(scene);
	memdelete(instance);
}

TEST_CASE("[PackedScene] Runtime plan customization preserves persistent connections for duplicated subtrees") {
	PlanningNode *scene = PlanningNode::create_registered();
	scene->set_name("Scene");
	scene->set_duplicate_count(2);

	PlanningLeaf *template_root = PlanningLeaf::create_registered();
	template_root->set_name("Template");
	template_root->set_number(99);
	scene->add_child(template_root);
	template_root->set_owner(scene);

	ConnectionEmitterNode *emitter = ConnectionEmitterNode::create_registered();
	emitter->set_name("Emitter");
	template_root->add_child(emitter);
	emitter->set_owner(scene);

	ConnectionReceiverNode *receiver = ConnectionReceiverNode::create_registered();
	receiver->set_name("Receiver");
	template_root->add_child(receiver);
	receiver->set_owner(scene);

	CHECK_EQ(emitter->connect("ping", Callable(receiver, "mark_received"), Object::CONNECT_PERSIST), OK);

	PackedScene packed_scene;
	CHECK_EQ(packed_scene.pack(scene), OK);

	Node *instance = packed_scene.instantiate();
	REQUIRE(instance != nullptr);

	PlanningLeaf *template_0 = Object::cast_to<PlanningLeaf>(instance->get_node_or_null(NodePath("Template0")));
	PlanningLeaf *template_1 = Object::cast_to<PlanningLeaf>(instance->get_node_or_null(NodePath("Template1")));
	REQUIRE(template_0 != nullptr);
	REQUIRE(template_1 != nullptr);

	ConnectionEmitterNode *emitter_0 = Object::cast_to<ConnectionEmitterNode>(template_0->get_node_or_null(NodePath("Emitter")));
	ConnectionReceiverNode *receiver_0 = Object::cast_to<ConnectionReceiverNode>(template_0->get_node_or_null(NodePath("Receiver")));
	ConnectionEmitterNode *emitter_1 = Object::cast_to<ConnectionEmitterNode>(template_1->get_node_or_null(NodePath("Emitter")));
	ConnectionReceiverNode *receiver_1 = Object::cast_to<ConnectionReceiverNode>(template_1->get_node_or_null(NodePath("Receiver")));
	REQUIRE(emitter_0 != nullptr);
	REQUIRE(receiver_0 != nullptr);
	REQUIRE(emitter_1 != nullptr);
	REQUIRE(receiver_1 != nullptr);

	CHECK(emitter_0->is_connected("ping", Callable(receiver_0, "mark_received")));
	CHECK_FALSE(emitter_0->is_connected("ping", Callable(receiver_1, "mark_received")));
	CHECK(emitter_1->is_connected("ping", Callable(receiver_1, "mark_received")));
	CHECK_FALSE(emitter_1->is_connected("ping", Callable(receiver_0, "mark_received")));

	CHECK_FALSE(receiver_0->was_received());
	CHECK_FALSE(receiver_1->was_received());
	emitter_0->emit_ping();
	CHECK(receiver_0->was_received());
	CHECK_FALSE(receiver_1->was_received());

	emitter_1->emit_ping();
	CHECK(receiver_1->was_received());

	memdelete(scene);
	memdelete(instance);
}

TEST_CASE("[PackedScene] Runtime plan customization preserves persistent connections after reload") {
	PlanningNode *scene = PlanningNode::create_registered();
	scene->set_name("Scene");

	ConnectionEmitterNode *emitter = ConnectionEmitterNode::create_registered();
	emitter->set_name("Emitter");
	scene->add_child(emitter);
	emitter->set_owner(scene);

	ConnectionReceiverNode *receiver = ConnectionReceiverNode::create_registered();
	receiver->set_name("Receiver");
	scene->add_child(receiver);
	receiver->set_owner(scene);

	CHECK_EQ(emitter->connect("ping", Callable(receiver, "mark_received"), Object::CONNECT_PERSIST), OK);

	Ref<PackedScene> packed_scene;
	packed_scene.instantiate();
	CHECK_EQ(packed_scene->pack(scene), OK);

	const String scene_path = TestUtils::get_temp_path("runtime_plan_persistent_connections_reload.tscn");
	CHECK_EQ(ResourceSaver::save(packed_scene, scene_path), OK);

	Error err = OK;
	Ref<PackedScene> reloaded_scene = ResourceLoader::load(scene_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(reloaded_scene.is_valid());

	Node *instance = reloaded_scene->instantiate();
	REQUIRE(instance != nullptr);

	ConnectionEmitterNode *instanced_emitter = Object::cast_to<ConnectionEmitterNode>(instance->get_node_or_null(NodePath("Emitter")));
	ConnectionReceiverNode *instanced_receiver = Object::cast_to<ConnectionReceiverNode>(instance->get_node_or_null(NodePath("Receiver")));
	REQUIRE(instanced_emitter != nullptr);
	REQUIRE(instanced_receiver != nullptr);
	CHECK(instanced_emitter->is_connected("ping", Callable(instanced_receiver, "mark_received")));

	instanced_emitter->emit_ping();
	CHECK(instanced_receiver->was_received());

	memdelete(scene);
	memdelete(instance);
	DirAccess::remove_file_or_error(scene_path);
}

TEST_CASE("[PackedScene] Runtime plan extraction can defer subtree instantiation") {
	ExtractingPlanningNode *scene = ExtractingPlanningNode::create_registered();
	scene->set_name("Scene");

	Node *deferred_root = memnew(Node);
	deferred_root->set_name("Deferred");
	scene->add_child(deferred_root);
	deferred_root->set_owner(scene);

	ConnectionEmitterNode *emitter = ConnectionEmitterNode::create_registered();
	emitter->set_name("Emitter");
	deferred_root->add_child(emitter);
	emitter->set_owner(scene);

	ConnectionReceiverNode *receiver = ConnectionReceiverNode::create_registered();
	receiver->set_name("Receiver");
	deferred_root->add_child(receiver);
	receiver->set_owner(scene);

	CHECK_EQ(emitter->connect("ping", Callable(receiver, "mark_received"), Object::CONNECT_PERSIST), OK);

	PackedScene packed_scene;
	CHECK_EQ(packed_scene.pack(scene), OK);

	ExtractingPlanningNode *instance = Object::cast_to<ExtractingPlanningNode>(packed_scene.instantiate());
	REQUIRE(instance != nullptr);
	CHECK(instance->get_node_or_null(NodePath("Deferred")) == nullptr);

	Ref<PackedScene> extracted_scene = instance->get_extracted_scene();
	REQUIRE(extracted_scene.is_valid());

	Node *deferred_instance_a = extracted_scene->instantiate();
	Node *deferred_instance_b = extracted_scene->instantiate();
	REQUIRE(deferred_instance_a != nullptr);
	REQUIRE(deferred_instance_b != nullptr);

	ConnectionEmitterNode *deferred_emitter_a = Object::cast_to<ConnectionEmitterNode>(deferred_instance_a->get_node_or_null(NodePath("Emitter")));
	ConnectionReceiverNode *deferred_receiver_a = Object::cast_to<ConnectionReceiverNode>(deferred_instance_a->get_node_or_null(NodePath("Receiver")));
	REQUIRE(deferred_emitter_a != nullptr);
	REQUIRE(deferred_receiver_a != nullptr);
	CHECK(deferred_emitter_a->is_connected("ping", Callable(deferred_receiver_a, "mark_received")));
	deferred_emitter_a->emit_ping();
	CHECK(deferred_receiver_a->was_received());

	ConnectionEmitterNode *deferred_emitter_b = Object::cast_to<ConnectionEmitterNode>(deferred_instance_b->get_node_or_null(NodePath("Emitter")));
	ConnectionReceiverNode *deferred_receiver_b = Object::cast_to<ConnectionReceiverNode>(deferred_instance_b->get_node_or_null(NodePath("Receiver")));
	REQUIRE(deferred_emitter_b != nullptr);
	REQUIRE(deferred_receiver_b != nullptr);
	CHECK(deferred_emitter_b->is_connected("ping", Callable(deferred_receiver_b, "mark_received")));

	memdelete(scene);
	memdelete(instance);
	memdelete(deferred_instance_a);
	memdelete(deferred_instance_b);
}

TEST_CASE("[PackedScene] Runtime plan extraction preserves nested scene descendants") {
	Node *nested_root = memnew(Node);
	nested_root->set_name("NestedRoot");

	PlanningLeaf *nested_leaf = PlanningLeaf::create_registered();
	nested_leaf->set_name("NestedLeaf");
	nested_leaf->set_number(42);
	nested_root->add_child(nested_leaf);
	nested_leaf->set_owner(nested_root);

	Ref<PackedScene> nested_scene;
	nested_scene.instantiate();
	CHECK_EQ(nested_scene->pack(nested_root), OK);

	const String nested_scene_path = TestUtils::get_temp_path("runtime_plan_extract_nested_scene_base.tscn");
	CHECK_EQ(ResourceSaver::save(nested_scene, nested_scene_path), OK);

	Error err = OK;
	Ref<PackedScene> nested_scene_loaded = ResourceLoader::load(nested_scene_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	CHECK_EQ(err, OK);
	REQUIRE(nested_scene_loaded.is_valid());

	ExtractingPlanningNode *scene = ExtractingPlanningNode::create_registered();
	scene->set_name("Scene");

	Node *deferred_root = memnew(Node);
	deferred_root->set_name("Deferred");
	scene->add_child(deferred_root);
	deferred_root->set_owner(scene);

	Node *nested_instance = nested_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);
	REQUIRE(nested_instance != nullptr);
	deferred_root->add_child(nested_instance);
	nested_instance->set_owner(scene);

	PackedScene packed_scene;
	CHECK_EQ(packed_scene.pack(scene), OK);

	ExtractingPlanningNode *instance = Object::cast_to<ExtractingPlanningNode>(packed_scene.instantiate());
	REQUIRE(instance != nullptr);
	CHECK(instance->get_node_or_null(NodePath("Deferred")) == nullptr);

	Ref<PackedScene> extracted_scene = instance->get_extracted_scene();
	REQUIRE(extracted_scene.is_valid());

	Ref<SceneState> extracted_state = extracted_scene->get_state();
	REQUIRE(extracted_state.is_valid());
	CHECK_EQ(extracted_state->get_node_count(), 3);

	Node *deferred_instance = extracted_scene->instantiate();
	REQUIRE(deferred_instance != nullptr);

	PlanningLeaf *extracted_leaf = Object::cast_to<PlanningLeaf>(deferred_instance->get_node_or_null(NodePath("NestedRoot/NestedLeaf")));
	REQUIRE(extracted_leaf != nullptr);
	if (extracted_leaf != nullptr) {
		CHECK_EQ(extracted_leaf->get_number(), 42);
	}

	memdelete(nested_root);
	memdelete(scene);
	memdelete(instance);
	memdelete(deferred_instance);
	DirAccess::remove_file_or_error(nested_scene_path);
}

TEST_CASE("[PackedScene] Runtime plan instantiation preserves nested scene descendants") {
	Node *nested_root = memnew(Node);
	nested_root->set_name("NestedRoot");

	PlanningLeaf *nested_leaf = PlanningLeaf::create_registered();
	nested_leaf->set_name("NestedLeaf");
	nested_leaf->set_number(42);
	nested_root->add_child(nested_leaf);
	nested_leaf->set_owner(nested_root);

	Ref<PackedScene> nested_scene;
	nested_scene.instantiate();
	CHECK_EQ(nested_scene->pack(nested_root), OK);

	const String nested_scene_path = TestUtils::get_temp_path("runtime_plan_noop_nested_scene_base.tscn");
	CHECK_EQ(ResourceSaver::save(nested_scene, nested_scene_path), OK);

	Error err = OK;
	Ref<PackedScene> nested_scene_loaded = ResourceLoader::load(nested_scene_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	CHECK_EQ(err, OK);
	REQUIRE(nested_scene_loaded.is_valid());

	NoopPlanningNode *scene = NoopPlanningNode::create_registered();
	scene->set_name("Scene");

	Node *deferred_root = memnew(Node);
	deferred_root->set_name("Deferred");
	scene->add_child(deferred_root);
	deferred_root->set_owner(scene);

	Node *nested_instance = nested_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);
	REQUIRE(nested_instance != nullptr);
	deferred_root->add_child(nested_instance);
	nested_instance->set_owner(scene);

	PackedScene packed_scene;
	CHECK_EQ(packed_scene.pack(scene), OK);

	NoopPlanningNode *instance = Object::cast_to<NoopPlanningNode>(packed_scene.instantiate());
	REQUIRE(instance != nullptr);

	PlanningLeaf *instanced_leaf = Object::cast_to<PlanningLeaf>(instance->get_node_or_null(NodePath("Deferred/NestedRoot/NestedLeaf")));
	REQUIRE(instanced_leaf != nullptr);
	if (instanced_leaf != nullptr) {
		CHECK_EQ(instanced_leaf->get_number(), 42);
	}

	memdelete(nested_root);
	memdelete(scene);
	memdelete(instance);
	DirAccess::remove_file_or_error(nested_scene_path);
}

TEST_CASE("[PackedScene] Runtime plan customization preserves root override connections for instanced scene roots") {
	ConnectionReceiverNode *base_root = ConnectionReceiverNode::create_registered();
	base_root->set_name("BaseReceiver");

	Ref<PackedScene> base_scene;
	base_scene.instantiate();
	CHECK_EQ(base_scene->pack(base_root), OK);

	const String base_path = TestUtils::get_temp_path("runtime_plan_root_override_connections_base.tscn");
	CHECK_EQ(ResourceSaver::save(base_scene, base_path), OK);

	Error err = OK;
	Ref<PackedScene> base_scene_loaded = ResourceLoader::load(base_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(base_scene_loaded.is_valid());

	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	ConnectionReceiverNode *override_root = Object::cast_to<ConnectionReceiverNode>(base_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN));
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(override_root != nullptr);
	override_root->set_name("WrappedInstance");

	ConnectionEmitterNode *override_emitter = ConnectionEmitterNode::create_registered();
	override_emitter->set_name("Emitter");
	override_root->add_child(override_emitter);
	override_emitter->set_owner(override_root);
	CHECK_EQ(override_emitter->connect("ping", Callable(override_root, "mark_received"), Object::CONNECT_PERSIST), OK);

	Ref<PackedScene> override_scene;
	override_scene.instantiate();
	Engine::get_singleton()->set_editor_hint(true);
	CHECK_EQ(override_scene->pack(override_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	const String override_path = TestUtils::get_temp_path("runtime_plan_root_override_connections_wrapper.tscn");
	CHECK_EQ(ResourceSaver::save(override_scene, override_path), OK);

	Ref<PackedScene> override_scene_loaded = ResourceLoader::load(override_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(override_scene_loaded.is_valid());

	PlanningNode *outer_root = PlanningNode::create_registered();
	outer_root->set_name("OuterRoot");

	Engine::get_singleton()->set_editor_hint(true);
	Node *nested_instance = override_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(nested_instance != nullptr);
	outer_root->add_child(nested_instance);
	nested_instance->set_owner(outer_root);

	PackedScene outer_scene;
	Engine::get_singleton()->set_editor_hint(true);
	CHECK_EQ(outer_scene.pack(outer_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = outer_scene.instantiate();
	REQUIRE(instance != nullptr);

	ConnectionReceiverNode *instanced_root = Object::cast_to<ConnectionReceiverNode>(instance->get_node_or_null(NodePath("WrappedInstance")));
	REQUIRE(instanced_root != nullptr);

	ConnectionEmitterNode *instanced_emitter = Object::cast_to<ConnectionEmitterNode>(instanced_root->get_node_or_null(NodePath("Emitter")));
	REQUIRE(instanced_emitter != nullptr);
	CHECK(instanced_emitter->is_connected("ping", Callable(instanced_root, "mark_received")));

	instanced_emitter->emit_ping();
	CHECK(instanced_root->was_received());

	memdelete(base_root);
	memdelete(override_root);
	memdelete(outer_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(base_path);
	DirAccess::remove_file_or_error(override_path);
}

TEST_CASE("[PackedScene] Runtime plan customization preserves stacked root override connections for instanced scene roots") {
	ConnectionReceiverNode *base_root = ConnectionReceiverNode::create_registered();
	base_root->set_name("BaseReceiver");

	Ref<PackedScene> base_scene;
	base_scene.instantiate();
	CHECK_EQ(base_scene->pack(base_root), OK);

	const String base_path = TestUtils::get_temp_path("runtime_plan_stacked_root_override_connections_base.tscn");
	CHECK_EQ(ResourceSaver::save(base_scene, base_path), OK);

	Error err = OK;
	Ref<PackedScene> base_scene_loaded = ResourceLoader::load(base_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(base_scene_loaded.is_valid());

	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	ConnectionReceiverNode *inner_override_root = Object::cast_to<ConnectionReceiverNode>(base_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN));
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(inner_override_root != nullptr);
	inner_override_root->set_name("WrappedTwice");

	ConnectionEmitterNode *inner_emitter = ConnectionEmitterNode::create_registered();
	inner_emitter->set_name("Emitter");
	inner_override_root->add_child(inner_emitter);
	inner_emitter->set_owner(inner_override_root);
	CHECK_EQ(inner_emitter->connect("ping", Callable(inner_override_root, "mark_received"), Object::CONNECT_PERSIST), OK);

	Ref<PackedScene> inner_override_scene;
	inner_override_scene.instantiate();
	Engine::get_singleton()->set_editor_hint(true);
	CHECK_EQ(inner_override_scene->pack(inner_override_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	const String inner_override_path = TestUtils::get_temp_path("runtime_plan_stacked_root_override_connections_inner_wrapper.tscn");
	CHECK_EQ(ResourceSaver::save(inner_override_scene, inner_override_path), OK);

	Ref<PackedScene> inner_override_scene_loaded = ResourceLoader::load(inner_override_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(inner_override_scene_loaded.is_valid());

	Engine::get_singleton()->set_editor_hint(true);
	ConnectionReceiverNode *outer_override_root = Object::cast_to<ConnectionReceiverNode>(inner_override_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN));
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(outer_override_root != nullptr);
	outer_override_root->set_name("WrappedTwice");

	PlanningLeaf *outer_leaf = PlanningLeaf::create_registered();
	outer_leaf->set_name("OuterLeaf");
	outer_leaf->set_number(42);
	outer_override_root->add_child(outer_leaf);
	outer_leaf->set_owner(outer_override_root);

	Ref<PackedScene> outer_override_scene;
	outer_override_scene.instantiate();
	Engine::get_singleton()->set_editor_hint(true);
	CHECK_EQ(outer_override_scene->pack(outer_override_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	const String outer_override_path = TestUtils::get_temp_path("runtime_plan_stacked_root_override_connections_outer_wrapper.tscn");
	CHECK_EQ(ResourceSaver::save(outer_override_scene, outer_override_path), OK);

	Ref<PackedScene> outer_override_scene_loaded = ResourceLoader::load(outer_override_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(outer_override_scene_loaded.is_valid());

	PlanningNode *main_root = PlanningNode::create_registered();
	main_root->set_name("MainRoot");

	Engine::get_singleton()->set_editor_hint(true);
	Node *nested_instance = outer_override_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(nested_instance != nullptr);
	main_root->add_child(nested_instance);
	nested_instance->set_owner(main_root);

	PackedScene main_scene;
	Engine::get_singleton()->set_editor_hint(true);
	CHECK_EQ(main_scene.pack(main_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = main_scene.instantiate();
	REQUIRE(instance != nullptr);

	ConnectionReceiverNode *instanced_root = Object::cast_to<ConnectionReceiverNode>(instance->get_node_or_null(NodePath("WrappedTwice")));
	REQUIRE(instanced_root != nullptr);

	PlanningLeaf *instanced_outer_leaf = Object::cast_to<PlanningLeaf>(instanced_root->get_node_or_null(NodePath("OuterLeaf")));
	REQUIRE(instanced_outer_leaf != nullptr);
	CHECK_EQ(instanced_outer_leaf->get_number(), 42);

	ConnectionEmitterNode *instanced_emitter = Object::cast_to<ConnectionEmitterNode>(instanced_root->get_node_or_null(NodePath("Emitter")));
	REQUIRE(instanced_emitter != nullptr);
	CHECK(instanced_emitter->is_connected("ping", Callable(instanced_root, "mark_received")));

	instanced_emitter->emit_ping();
	CHECK(instanced_root->was_received());

	memdelete(base_root);
	memdelete(inner_override_root);
	memdelete(outer_override_root);
	memdelete(main_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(base_path);
	DirAccess::remove_file_or_error(inner_override_path);
	DirAccess::remove_file_or_error(outer_override_path);
}

TEST_CASE("[PackedScene] Runtime plan customization duplicates local scene resources") {
	PlanningNode *scene = PlanningNode::create_registered();
	scene->set_name("Scene");

	Ref<Resource> payload;
	payload.instantiate();
	payload->set_local_to_scene(true);
	payload->set_name("Payload");

	ResourceHolderNode *holder = ResourceHolderNode::create_registered();
	holder->set_name("Holder");
	holder->set_payload(payload);
	scene->add_child(holder);
	holder->set_owner(scene);

	PackedScene packed_scene;
	CHECK_EQ(packed_scene.pack(scene), OK);

	Node *instance_a = packed_scene.instantiate();
	Node *instance_b = packed_scene.instantiate();
	REQUIRE(instance_a != nullptr);
	REQUIRE(instance_b != nullptr);

	ResourceHolderNode *holder_a = Object::cast_to<ResourceHolderNode>(instance_a->get_node_or_null(NodePath("Holder")));
	ResourceHolderNode *holder_b = Object::cast_to<ResourceHolderNode>(instance_b->get_node_or_null(NodePath("Holder")));
	REQUIRE(holder_a != nullptr);
	REQUIRE(holder_b != nullptr);

	Ref<Resource> payload_a = holder_a->get_payload();
	Ref<Resource> payload_b = holder_b->get_payload();
	REQUIRE(payload_a.is_valid());
	REQUIRE(payload_b.is_valid());
	CHECK(payload_a != payload);
	CHECK(payload_b != payload);
	CHECK(payload_a != payload_b);
	CHECK_EQ(payload_a->get_local_scene(), instance_a);
	CHECK_EQ(payload_b->get_local_scene(), instance_b);

	memdelete(scene);
	memdelete(instance_a);
	memdelete(instance_b);
}

TEST_CASE("[PackedScene] Nested runtime plan customization merges instance children and owner overrides") {
	NestedChoosingNode *internal_root = NestedChoosingNode::create_registered();
	internal_root->set_name("NestedRoot");
	internal_root->set_kept_child_name("TemplateKeep");

	PlanningLeaf *internal_keep = PlanningLeaf::create_registered();
	internal_keep->set_name("TemplateKeep");
	internal_keep->set_number(10);
	internal_root->add_child(internal_keep);
	internal_keep->set_owner(internal_root);

	PlanningLeaf *internal_drop = PlanningLeaf::create_registered();
	internal_drop->set_name("TemplateDrop");
	internal_drop->set_number(20);
	internal_root->add_child(internal_drop);
	internal_drop->set_owner(internal_root);

	Ref<PackedScene> internal_scene;
	internal_scene.instantiate();
	CHECK_EQ(internal_scene->pack(internal_root), OK);

	const String internal_path = TestUtils::get_temp_path("nested_runtime_plan_internal.tscn");
	CHECK_EQ(ResourceSaver::save(internal_scene, internal_path), OK);

	Error err = OK;
	Ref<PackedScene> internal_scene_loaded = ResourceLoader::load(internal_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(internal_scene_loaded.is_valid());

	Node *main_root = memnew(Node);
	main_root->set_name("MainRoot");
	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);

	NestedChoosingNode *nested_instance = Object::cast_to<NestedChoosingNode>(internal_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN));
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(nested_instance != nullptr);
	nested_instance->set_name("NestedInstance");
	nested_instance->set_kept_child_name("OwnerAdded");
	main_root->add_child(nested_instance);
	nested_instance->set_owner(main_root);

	PlanningLeaf *owner_added = PlanningLeaf::create_registered();
	owner_added->set_name("OwnerAdded");
	owner_added->set_number(42);
	nested_instance->add_child(owner_added);
	owner_added->set_owner(main_root);

	Engine::get_singleton()->set_editor_hint(true);
	PackedScene main_scene;
	CHECK_EQ(main_scene.pack(main_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = main_scene.instantiate();
	REQUIRE(instance != nullptr);

	NestedChoosingNode *instanced_nested = Object::cast_to<NestedChoosingNode>(instance->get_node_or_null(NodePath("NestedInstance")));
	REQUIRE(instanced_nested != nullptr);
	CHECK_EQ(instanced_nested->get_child_count(), 1);

	PlanningLeaf *surviving_child = Object::cast_to<PlanningLeaf>(instanced_nested->get_child(0));
	REQUIRE(surviving_child != nullptr);
	CHECK_EQ(surviving_child->get_name(), StringName("OwnerAdded"));
	CHECK_EQ(surviving_child->get_number(), 42);

	memdelete(internal_root);
	memdelete(main_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(internal_path);
}

TEST_CASE("[PackedScene] Nested runtime plan customization sees internal overridden child properties") {
	NestedPropertySelectingNode *internal_root = NestedPropertySelectingNode::create_registered();
	internal_root->set_name("NestedRoot");
	internal_root->set_required_number(99);

	PlanningLeaf *internal_keep = PlanningLeaf::create_registered();
	internal_keep->set_name("TemplateKeep");
	internal_keep->set_number(10);
	internal_root->add_child(internal_keep);
	internal_keep->set_owner(internal_root);

	PlanningLeaf *internal_drop = PlanningLeaf::create_registered();
	internal_drop->set_name("TemplateDrop");
	internal_drop->set_number(20);
	internal_root->add_child(internal_drop);
	internal_drop->set_owner(internal_root);

	Ref<PackedScene> internal_scene;
	internal_scene.instantiate();
	CHECK_EQ(internal_scene->pack(internal_root), OK);

	const String internal_path = TestUtils::get_temp_path("nested_runtime_plan_internal_override.tscn");
	CHECK_EQ(ResourceSaver::save(internal_scene, internal_path), OK);

	Error err = OK;
	Ref<PackedScene> internal_scene_loaded = ResourceLoader::load(internal_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(internal_scene_loaded.is_valid());

	Node *main_root = memnew(Node);
	main_root->set_name("MainRoot");
	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);

	NestedPropertySelectingNode *nested_instance = Object::cast_to<NestedPropertySelectingNode>(internal_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN));
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(nested_instance != nullptr);
	nested_instance->set_name("NestedInstance");
	nested_instance->set_customization_enabled(true);
	main_root->add_child(nested_instance);
	nested_instance->set_owner(main_root);
	main_root->set_editable_instance(nested_instance, true);

	PlanningLeaf *overridden_child = Object::cast_to<PlanningLeaf>(nested_instance->get_node_or_null(NodePath("TemplateKeep")));
	REQUIRE(overridden_child != nullptr);
	overridden_child->set_number(99);

	Engine::get_singleton()->set_editor_hint(true);
	PackedScene main_scene;
	CHECK_EQ(main_scene.pack(main_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = main_scene.instantiate();
	REQUIRE(instance != nullptr);

	NestedPropertySelectingNode *instanced_nested = Object::cast_to<NestedPropertySelectingNode>(instance->get_node_or_null(NodePath("NestedInstance")));
	REQUIRE(instanced_nested != nullptr);
	REQUIRE_EQ(instanced_nested->get_child_count(), 1);

	PlanningLeaf *surviving_child = Object::cast_to<PlanningLeaf>(instanced_nested->get_child(0));
	REQUIRE(surviving_child != nullptr);
	CHECK_EQ(surviving_child->get_name(), StringName("TemplateKeep"));
	CHECK_EQ(surviving_child->get_number(), 99);

	memdelete(internal_root);
	memdelete(main_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(internal_path);
}

TEST_CASE("[PackedScene] Nested runtime plan customization keeps owner-added instance children") {
	Node *child_root = memnew(Node);
	child_root->set_name("ChildRoot");

	PlanningLeaf *child_payload = PlanningLeaf::create_registered();
	child_payload->set_name("Payload");
	child_payload->set_number(7);
	child_root->add_child(child_payload);
	child_payload->set_owner(child_root);

	Ref<PackedScene> child_scene;
	child_scene.instantiate();
	CHECK_EQ(child_scene->pack(child_root), OK);

	const String child_path = TestUtils::get_temp_path("nested_runtime_plan_owner_added_instance_child.tscn");
	CHECK_EQ(ResourceSaver::save(child_scene, child_path), OK);

	Error err = OK;
	Ref<PackedScene> child_scene_loaded = ResourceLoader::load(child_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(child_scene_loaded.is_valid());

	NestedChoosingNode *outer_root = NestedChoosingNode::create_registered();
	outer_root->set_name("OuterRoot");
	outer_root->set_kept_child_name("ChildInstance");

	Ref<PackedScene> outer_scene;
	outer_scene.instantiate();
	CHECK_EQ(outer_scene->pack(outer_root), OK);

	const String outer_path = TestUtils::get_temp_path("nested_runtime_plan_owner_added_instance_outer.tscn");
	CHECK_EQ(ResourceSaver::save(outer_scene, outer_path), OK);

	err = OK;
	Ref<PackedScene> outer_scene_loaded = ResourceLoader::load(outer_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(outer_scene_loaded.is_valid());

	Node *main_root = memnew(Node);
	main_root->set_name("MainRoot");
	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);

	NestedChoosingNode *outer_instance = Object::cast_to<NestedChoosingNode>(outer_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN));
	REQUIRE(outer_instance != nullptr);
	outer_instance->set_name("OuterInstance");
	main_root->add_child(outer_instance);
	outer_instance->set_owner(main_root);

	Node *child_instance = child_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(child_instance != nullptr);
	child_instance->set_name("ChildInstance");
	outer_instance->add_child(child_instance);
	child_instance->set_owner(main_root);

	Engine::get_singleton()->set_editor_hint(true);
	PackedScene main_scene;
	CHECK_EQ(main_scene.pack(main_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = main_scene.instantiate();
	REQUIRE(instance != nullptr);

	NestedChoosingNode *instanced_outer = Object::cast_to<NestedChoosingNode>(instance->get_node_or_null(NodePath("OuterInstance")));
	REQUIRE(instanced_outer != nullptr);
	REQUIRE_EQ(instanced_outer->get_child_count(), 1);

	Node *instanced_child = instanced_outer->get_child(0);
	REQUIRE(instanced_child != nullptr);
	CHECK_EQ(instanced_child->get_name(), StringName("ChildInstance"));
	CHECK(instanced_child->get_node_or_null(NodePath("Payload")) != nullptr);

	memdelete(child_root);
	memdelete(outer_root);
	memdelete(main_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(child_path);
	DirAccess::remove_file_or_error(outer_path);
}

TEST_CASE("[PackedScene] Nested runtime plan keeps owner-added instance children attached to the instance root") {
	Node *child_root = memnew(Node);
	child_root->set_name("ChildRoot");

	Node *same_name_child = memnew(Node);
	same_name_child->set_name("ParentInstance");
	child_root->add_child(same_name_child);
	same_name_child->set_owner(child_root);

	PlanningLeaf *child_payload = PlanningLeaf::create_registered();
	child_payload->set_name("Payload");
	child_payload->set_number(7);
	child_root->add_child(child_payload);
	child_payload->set_owner(child_root);

	Ref<PackedScene> child_scene;
	child_scene.instantiate();
	CHECK_EQ(child_scene->pack(child_root), OK);

	const String child_path = TestUtils::get_temp_path("nested_runtime_plan_owner_added_instance_same_name_child.tscn");
	CHECK_EQ(ResourceSaver::save(child_scene, child_path), OK);

	Error err = OK;
	Ref<PackedScene> child_scene_loaded = ResourceLoader::load(child_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(child_scene_loaded.is_valid());

	NestedChoosingNode *main_root = NestedChoosingNode::create_registered();
	main_root->set_name("MainRoot");
	main_root->set_kept_child_name("ParentInstance");

	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);

	Node *parent_instance = child_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	REQUIRE(parent_instance != nullptr);
	parent_instance->set_name("ParentInstance");
	main_root->add_child(parent_instance);
	parent_instance->set_owner(main_root);

	Node *nested_instance = child_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(nested_instance != nullptr);
	nested_instance->set_name("NestedInstance");
	parent_instance->add_child(nested_instance);
	nested_instance->set_owner(main_root);

	PackedScene main_scene;
	CHECK_EQ(main_scene.pack(main_root), OK);

	Node *instance = main_scene.instantiate();
	REQUIRE(instance != nullptr);

	Node *instanced_parent = instance->get_node_or_null(NodePath("ParentInstance"));
	REQUIRE(instanced_parent != nullptr);
	CHECK(instanced_parent->get_node_or_null(NodePath("ParentInstance")) != nullptr);

	Node *direct_nested = instanced_parent->get_node_or_null(NodePath("NestedInstance"));
	REQUIRE(direct_nested != nullptr);
	if (direct_nested != nullptr) {
		CHECK_EQ(direct_nested->get_parent(), instanced_parent);
	}
	CHECK(instanced_parent->get_node_or_null(NodePath("ParentInstance/NestedInstance")) == nullptr);
	if (direct_nested != nullptr) {
		CHECK(direct_nested->get_node_or_null(NodePath("Payload")) != nullptr);
	}

	memdelete(child_root);
	memdelete(main_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(child_path);
}

TEST_CASE("[PackedScene] Nested runtime plan customization keeps multiple owner-added instance siblings") {
	Node *child_root = memnew(Node);
	child_root->set_name("ChildRoot");

	PlanningLeaf *child_payload = PlanningLeaf::create_registered();
	child_payload->set_name("Payload");
	child_payload->set_number(7);
	child_root->add_child(child_payload);
	child_payload->set_owner(child_root);

	Ref<PackedScene> child_scene;
	child_scene.instantiate();
	CHECK_EQ(child_scene->pack(child_root), OK);

	const String child_path = TestUtils::get_temp_path("nested_runtime_plan_owner_added_instance_siblings_child.tscn");
	CHECK_EQ(ResourceSaver::save(child_scene, child_path), OK);

	Error err = OK;
	Ref<PackedScene> child_scene_loaded = ResourceLoader::load(child_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(child_scene_loaded.is_valid());

	Node *outer_root = memnew(Node);
	outer_root->set_name("OuterRoot");

	Ref<PackedScene> outer_scene;
	outer_scene.instantiate();
	CHECK_EQ(outer_scene->pack(outer_root), OK);

	const String outer_path = TestUtils::get_temp_path("nested_runtime_plan_owner_added_instance_siblings_outer.tscn");
	CHECK_EQ(ResourceSaver::save(outer_scene, outer_path), OK);

	err = OK;
	Ref<PackedScene> outer_scene_loaded = ResourceLoader::load(outer_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(outer_scene_loaded.is_valid());

	Node *main_root = memnew(Node);
	main_root->set_name("MainRoot");
	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);

	Node *outer_instance = outer_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	REQUIRE(outer_instance != nullptr);
	outer_instance->set_name("OuterInstance");
	main_root->add_child(outer_instance);
	outer_instance->set_owner(main_root);

	Node *child_instance_a = child_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	REQUIRE(child_instance_a != nullptr);
	child_instance_a->set_name("ChildA");
	outer_instance->add_child(child_instance_a);
	child_instance_a->set_owner(main_root);

	Node *child_instance_b = child_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(child_instance_b != nullptr);
	child_instance_b->set_name("ChildB");
	outer_instance->add_child(child_instance_b);
	child_instance_b->set_owner(main_root);

	Engine::get_singleton()->set_editor_hint(true);
	PackedScene main_scene;
	CHECK_EQ(main_scene.pack(main_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = main_scene.instantiate();
	REQUIRE(instance != nullptr);

	Node *instanced_outer = instance->get_node_or_null(NodePath("OuterInstance"));
	REQUIRE(instanced_outer != nullptr);

	Node *instanced_child_a = instanced_outer->get_node_or_null(NodePath("ChildA"));
	Node *instanced_child_b = instanced_outer->get_node_or_null(NodePath("ChildB"));
	REQUIRE(instanced_child_a != nullptr);
	REQUIRE(instanced_child_b != nullptr);
	CHECK(instanced_child_a->get_node_or_null(NodePath("Payload")) != nullptr);
	CHECK(instanced_child_b->get_node_or_null(NodePath("Payload")) != nullptr);

	memdelete(child_root);
	memdelete(outer_root);
	memdelete(main_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(child_path);
	DirAccess::remove_file_or_error(outer_path);
}

TEST_CASE("[PackedScene] Nested runtime plan customization runs for owner-added instance children") {
	NestedChoosingNode *child_root = NestedChoosingNode::create_registered();
	child_root->set_name("ChildRoot");
	child_root->set_kept_child_name("Keep");

	PlanningLeaf *keep_leaf = PlanningLeaf::create_registered();
	keep_leaf->set_name("Keep");
	child_root->add_child(keep_leaf);
	keep_leaf->set_owner(child_root);

	PlanningLeaf *drop_leaf = PlanningLeaf::create_registered();
	drop_leaf->set_name("Drop");
	child_root->add_child(drop_leaf);
	drop_leaf->set_owner(child_root);

	Ref<PackedScene> child_scene;
	child_scene.instantiate();
	CHECK_EQ(child_scene->pack(child_root), OK);

	const String child_path = TestUtils::get_temp_path("nested_runtime_plan_owner_added_customized_child.tscn");
	CHECK_EQ(ResourceSaver::save(child_scene, child_path), OK);

	Error err = OK;
	Ref<PackedScene> child_scene_loaded = ResourceLoader::load(child_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(child_scene_loaded.is_valid());

	NestedChoosingNode *outer_root = NestedChoosingNode::create_registered();
	outer_root->set_name("OuterRoot");
	outer_root->set_kept_child_name("ChildInstance");

	Ref<PackedScene> outer_scene;
	outer_scene.instantiate();
	CHECK_EQ(outer_scene->pack(outer_root), OK);

	const String outer_path = TestUtils::get_temp_path("nested_runtime_plan_owner_added_customized_outer.tscn");
	CHECK_EQ(ResourceSaver::save(outer_scene, outer_path), OK);

	err = OK;
	Ref<PackedScene> outer_scene_loaded = ResourceLoader::load(outer_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(outer_scene_loaded.is_valid());

	Node *main_root = memnew(Node);
	main_root->set_name("MainRoot");
	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);

	NestedChoosingNode *outer_instance = Object::cast_to<NestedChoosingNode>(outer_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN));
	REQUIRE(outer_instance != nullptr);
	outer_instance->set_name("OuterInstance");
	main_root->add_child(outer_instance);
	outer_instance->set_owner(main_root);

	Node *child_instance = child_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(child_instance != nullptr);
	child_instance->set_name("ChildInstance");
	outer_instance->add_child(child_instance);
	child_instance->set_owner(main_root);

	Engine::get_singleton()->set_editor_hint(true);
	PackedScene main_scene;
	CHECK_EQ(main_scene.pack(main_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = main_scene.instantiate();
	REQUIRE(instance != nullptr);

	NestedChoosingNode *instanced_outer = Object::cast_to<NestedChoosingNode>(instance->get_node_or_null(NodePath("OuterInstance")));
	REQUIRE(instanced_outer != nullptr);
	CHECK_EQ(instanced_outer->get_customization_call_count(), 1);
	REQUIRE_EQ(instanced_outer->get_child_count(), 1);

	NestedChoosingNode *instanced_child = Object::cast_to<NestedChoosingNode>(instanced_outer->get_node_or_null(NodePath("ChildInstance")));
	REQUIRE(instanced_child != nullptr);
	CHECK_EQ(instanced_child->get_customization_call_count(), 1);
	REQUIRE_EQ(instanced_child->get_child_count(), 1);
	CHECK(instanced_child->get_node_or_null(NodePath("Keep")) != nullptr);
	CHECK(instanced_child->get_node_or_null(NodePath("Drop")) == nullptr);

	memdelete(child_root);
	memdelete(outer_root);
	memdelete(main_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(child_path);
	DirAccess::remove_file_or_error(outer_path);
}

TEST_CASE("[PackedScene] Runtime plan customization ignores unrelated instanced overrides") {
	NestedChoosingNode *custom_root = NestedChoosingNode::create_registered();
	custom_root->set_name("CustomRoot");
	custom_root->set_kept_child_name("Keep");

	PlanningLeaf *keep_leaf = PlanningLeaf::create_registered();
	keep_leaf->set_name("Keep");
	custom_root->add_child(keep_leaf);
	keep_leaf->set_owner(custom_root);

	PlanningLeaf *drop_leaf = PlanningLeaf::create_registered();
	drop_leaf->set_name("Drop");
	custom_root->add_child(drop_leaf);
	drop_leaf->set_owner(custom_root);

	Ref<PackedScene> custom_scene;
	custom_scene.instantiate();
	CHECK_EQ(custom_scene->pack(custom_root), OK);

	const String custom_path = TestUtils::get_temp_path("runtime_plan_custom_instance_with_unrelated_overrides_child.tscn");
	CHECK_EQ(ResourceSaver::save(custom_scene, custom_path), OK);

	Error err = OK;
	Ref<PackedScene> custom_scene_loaded = ResourceLoader::load(custom_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(custom_scene_loaded.is_valid());

	NestedPropertySelectingNode *editable_root = NestedPropertySelectingNode::create_registered();
	editable_root->set_name("EditableRoot");

	PlanningLeaf *editable_leaf = PlanningLeaf::create_registered();
	editable_leaf->set_name("EditableLeaf");
	editable_leaf->set_number(1);
	editable_root->add_child(editable_leaf);
	editable_leaf->set_owner(editable_root);

	Ref<PackedScene> editable_scene;
	editable_scene.instantiate();
	CHECK_EQ(editable_scene->pack(editable_root), OK);

	const String editable_path = TestUtils::get_temp_path("runtime_plan_custom_instance_with_unrelated_overrides_editable.tscn");
	CHECK_EQ(ResourceSaver::save(editable_scene, editable_path), OK);

	err = OK;
	Ref<PackedScene> editable_scene_loaded = ResourceLoader::load(editable_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(editable_scene_loaded.is_valid());

	Node *main_root = memnew(Node);
	main_root->set_name("MainRoot");
	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);

	NestedChoosingNode *custom_instance = Object::cast_to<NestedChoosingNode>(custom_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN));
	REQUIRE(custom_instance != nullptr);
	custom_instance->set_name("CustomInstance");
	main_root->add_child(custom_instance);
	custom_instance->set_owner(main_root);

	NestedPropertySelectingNode *editable_instance = Object::cast_to<NestedPropertySelectingNode>(editable_scene_loaded->instantiate(PackedScene::GEN_EDIT_STATE_MAIN));
	Engine::get_singleton()->set_editor_hint(was_editor_hint);
	REQUIRE(editable_instance != nullptr);
	editable_instance->set_name("EditableInstance");
	main_root->add_child(editable_instance);
	editable_instance->set_owner(main_root);
	main_root->set_editable_instance(editable_instance, true);

	PlanningLeaf *overridden_leaf = Object::cast_to<PlanningLeaf>(editable_instance->get_node_or_null(NodePath("EditableLeaf")));
	REQUIRE(overridden_leaf != nullptr);
	overridden_leaf->set_number(5);

	Engine::get_singleton()->set_editor_hint(true);
	PackedScene main_scene;
	CHECK_EQ(main_scene.pack(main_root), OK);
	Engine::get_singleton()->set_editor_hint(was_editor_hint);

	Node *instance = main_scene.instantiate();
	REQUIRE(instance != nullptr);

	NestedChoosingNode *instanced_custom = Object::cast_to<NestedChoosingNode>(instance->get_node_or_null(NodePath("CustomInstance")));
	REQUIRE(instanced_custom != nullptr);
	CHECK_EQ(instanced_custom->get_customization_call_count(), 1);
	REQUIRE_EQ(instanced_custom->get_child_count(), 1);
	CHECK(instanced_custom->get_node_or_null(NodePath("Keep")) != nullptr);
	CHECK(instanced_custom->get_node_or_null(NodePath("Drop")) == nullptr);

	PlanningLeaf *instanced_overridden_leaf = Object::cast_to<PlanningLeaf>(instance->get_node_or_null(NodePath("EditableInstance/EditableLeaf")));
	REQUIRE(instanced_overridden_leaf != nullptr);
	CHECK_EQ(instanced_overridden_leaf->get_number(), 5);

	memdelete(custom_root);
	memdelete(editable_root);
	memdelete(main_root);
	memdelete(instance);
	DirAccess::remove_file_or_error(custom_path);
	DirAccess::remove_file_or_error(editable_path);
}

TEST_CASE("[PackedScene] Set Path") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Set a new path for the packed scene.
	const String new_path = "NewTestPath";
	packed_scene.set_path(new_path);

	// Check if the path has been set correctly.
	Ref<SceneState> state = packed_scene.get_state();
	CHECK(state.is_valid());
	CHECK(state->get_path() == new_path);

	memdelete(scene);
}

TEST_CASE("[PackedScene] Replace State") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Create another scene state to replace with.
	Ref<SceneState> new_state = memnew(SceneState);
	new_state->set_path("NewPath");

	// Replace the state.
	packed_scene.replace_state(new_state);

	// Check if the state has been replaced.
	Ref<SceneState> state = packed_scene.get_state();
	CHECK(state.is_valid());
	CHECK(state == new_state);

	memdelete(scene);
}

TEST_CASE("[PackedScene] Recreate State") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	Ref<PackedScene> packed_scene;
	packed_scene.instantiate();
	packed_scene->pack(scene);

	// Recreate the state.
	packed_scene->recreate_state();

	// Check if the state has been recreated.
	Ref<SceneState> state = packed_scene->get_state();
	CHECK(state.is_valid());
	CHECK(state->get_node_count() == 0); // Since the state was recreated, it should be empty.

	memdelete(scene);
}

} // namespace TestPackedScene
