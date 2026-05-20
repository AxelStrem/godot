/**************************************************************************/
/*  mesh_merge_tool.h                                                     */
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

#include "core/templates/hash_map.h"
#include "scene/resources/mesh.h"
#include "scene/resources/surface_tool.h"

class MeshMergeTool : public RefCounted {
	GDCLASS(MeshMergeTool, RefCounted);

	struct BucketKey {
		int surface_index = 0;
		uint64_t format = 0;
		Mesh::PrimitiveType primitive = Mesh::PRIMITIVE_TRIANGLES;
		uint64_t material_id = 0;

		bool operator==(const BucketKey &p_other) const {
			return surface_index == p_other.surface_index && format == p_other.format && primitive == p_other.primitive && material_id == p_other.material_id;
		}
	};

	struct BucketKeyHasher {
		static _FORCE_INLINE_ uint32_t hash(const BucketKey &p_key) {
			uint32_t hash = hash_fmix32(uint32_t(p_key.surface_index));
			hash = hash_murmur3_one_64(p_key.format, hash);
			hash = hash_murmur3_one_64(uint64_t(p_key.primitive), hash);
			hash = hash_murmur3_one_64(p_key.material_id, hash);
			return hash;
		}
	};

	struct SurfaceBucket {
		Ref<SurfaceTool> surface_tool;
		Ref<Material> material;
		String name;
	};

	HashMap<BucketKey, SurfaceBucket, BucketKeyHasher> surface_buckets;
	Vector<BucketKey> surface_order;
	HashMap<BucketKey, SurfaceBucket, BucketKeyHasher> shadow_surface_buckets;
	Vector<BucketKey> shadow_surface_order;
	bool merge_shadow_meshes = true;

	Error _append_mesh_to_buckets(const Ref<Mesh> &p_mesh, const Transform3D &p_transform, HashMap<BucketKey, SurfaceBucket, BucketKeyHasher> &r_buckets, Vector<BucketKey> &r_order);
	Ref<ArrayMesh> _commit_buckets(HashMap<BucketKey, SurfaceBucket, BucketKeyHasher> &r_buckets, const Vector<BucketKey> &p_order, uint64_t p_flags);

protected:
	static void _bind_methods();

public:
	void clear();

	void set_merge_shadow_meshes(bool p_enabled);
	bool is_merge_shadow_meshes_enabled() const;

	Error append_mesh(const Ref<Mesh> &p_mesh, const Transform3D &p_transform, const Ref<Mesh> &p_shadow_mesh = Ref<Mesh>());
	Ref<ArrayMesh> commit(uint64_t p_flags = 0);
	Ref<ArrayMesh> commit_shadow_mesh(uint64_t p_flags = 0);
};