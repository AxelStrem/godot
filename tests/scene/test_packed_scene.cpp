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
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "scene/resources/packed_scene.h"
#include "tests/test_utils.h"

namespace TestPackedScene {

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
