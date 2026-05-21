/**************************************************************************/
/*  mesh_merge_tool.cpp                                                   */
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

#include "mesh_merge_tool.h"

#include "core/object/class_db.h"

void MeshMergeTool::_bind_methods() {
	ClassDB::bind_method(D_METHOD("clear"), &MeshMergeTool::clear);
	ClassDB::bind_method(D_METHOD("set_merge_shadow_meshes", "enabled"), &MeshMergeTool::set_merge_shadow_meshes);
	ClassDB::bind_method(D_METHOD("is_merge_shadow_meshes_enabled"), &MeshMergeTool::is_merge_shadow_meshes_enabled);
	ClassDB::bind_method(D_METHOD("append_mesh", "mesh", "transform", "shadow_mesh"), &MeshMergeTool::append_mesh, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("commit", "flags"), &MeshMergeTool::commit, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("commit_shadow_mesh", "flags"), &MeshMergeTool::commit_shadow_mesh, DEFVAL(0));

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "merge_shadow_meshes"), "set_merge_shadow_meshes", "is_merge_shadow_meshes_enabled");
}

void MeshMergeTool::clear() {
	surface_buckets.clear();
	surface_order.clear();
	shadow_surface_buckets.clear();
	shadow_surface_order.clear();
}

void MeshMergeTool::set_merge_shadow_meshes(bool p_enabled) {
	merge_shadow_meshes = p_enabled;
}

bool MeshMergeTool::is_merge_shadow_meshes_enabled() const {
	return merge_shadow_meshes;
}

Error MeshMergeTool::_append_mesh_to_buckets(const Ref<Mesh> &p_mesh, const Transform3D &p_transform, HashMap<BucketKey, SurfaceBucket, BucketKeyHasher> &r_buckets, Vector<BucketKey> &r_order) {
	ERR_FAIL_COND_V_MSG(p_mesh.is_null(), ERR_INVALID_PARAMETER, "MeshMergeTool.append_mesh() requires a valid Mesh resource.");
	const ArrayMesh *source_array_mesh = Object::cast_to<ArrayMesh>(p_mesh.ptr());

	for (int surface_index = 0; surface_index < p_mesh->get_surface_count(); surface_index++) {
		if (p_mesh->surface_get_array_len(surface_index) <= 0) {
			continue;
		}

		Array arrays = p_mesh->surface_get_arrays(surface_index);
		if (arrays.is_empty()) {
			continue;
		}

		Vector<Vector3> vertices = arrays[Mesh::ARRAY_VERTEX];
		if (vertices.is_empty()) {
			continue;
		}

		Ref<Material> material = p_mesh->surface_get_material(surface_index);
		String surface_name;
		if (source_array_mesh != nullptr) {
			surface_name = source_array_mesh->surface_get_name(surface_index);
		}

		BucketKey key;
		key.surface_index = surface_index;
		key.format = p_mesh->surface_get_format(surface_index);
		key.primitive = p_mesh->surface_get_primitive_type(surface_index);
		key.material_id = material.is_valid() ? uint64_t(material->get_instance_id()) : uint64_t(0);

		SurfaceBucket *bucket = r_buckets.getptr(key);
		if (bucket == nullptr) {
			SurfaceBucket new_bucket;
			new_bucket.surface_tool.instantiate();
			new_bucket.material = material;
			new_bucket.name = surface_name;
			new_bucket.surface_tool->set_material(material);
			r_buckets.insert(key, new_bucket);
			r_order.push_back(key);
			bucket = r_buckets.getptr(key);
		}

		if (bucket->name.is_empty()) {
			bucket->name = surface_name;
		}

		bucket->surface_tool->append_from(p_mesh, surface_index, p_transform);
	}

	return OK;
}

Error MeshMergeTool::append_mesh(const Ref<Mesh> &p_mesh, const Transform3D &p_transform, const Ref<Mesh> &p_shadow_mesh) {
	Error err = _append_mesh_to_buckets(p_mesh, p_transform, surface_buckets, surface_order);
		ERR_FAIL_COND_V(err != OK, err);

	if (!merge_shadow_meshes) {
		return OK;
	}

	Ref<Mesh> effective_shadow_mesh = p_shadow_mesh;
	if (effective_shadow_mesh.is_null()) {
		const ArrayMesh *array_mesh = Object::cast_to<ArrayMesh>(p_mesh.ptr());
		if (array_mesh != nullptr) {
			effective_shadow_mesh = array_mesh->get_shadow_mesh();
		}
	}

	if (effective_shadow_mesh.is_valid()) {
		return _append_mesh_to_buckets(effective_shadow_mesh, p_transform, shadow_surface_buckets, shadow_surface_order);
	}

	return OK;
}

Ref<ArrayMesh> MeshMergeTool::_commit_buckets(HashMap<BucketKey, SurfaceBucket, BucketKeyHasher> &r_buckets, const Vector<BucketKey> &p_order, uint64_t p_flags) {
	if (p_order.is_empty()) {
		return Ref<ArrayMesh>();
	}

	Ref<ArrayMesh> mesh;
	mesh.instantiate();

	for (int i = 0; i < p_order.size(); i++) {
		SurfaceBucket *bucket = r_buckets.getptr(p_order[i]);
		if (bucket == nullptr || bucket->surface_tool.is_null()) {
			continue;
		}

		bucket->surface_tool->commit(mesh, p_flags);
		int merged_surface_index = mesh->get_surface_count() - 1;
		if (!bucket->name.is_empty()) {
			mesh->surface_set_name(merged_surface_index, bucket->name);
		}
	}

	return mesh;
}

Ref<ArrayMesh> MeshMergeTool::commit(uint64_t p_flags) {
	Ref<ArrayMesh> mesh = _commit_buckets(surface_buckets, surface_order, p_flags);
	if (mesh.is_null() || !merge_shadow_meshes) {
		return mesh;
	}

	Ref<ArrayMesh> shadow_mesh = _commit_buckets(shadow_surface_buckets, shadow_surface_order, p_flags);
	if (shadow_mesh.is_valid()) {
		mesh->set_shadow_mesh(shadow_mesh);
	}

	return mesh;
}

Ref<ArrayMesh> MeshMergeTool::commit_shadow_mesh(uint64_t p_flags) {
	return _commit_buckets(shadow_surface_buckets, shadow_surface_order, p_flags);
}