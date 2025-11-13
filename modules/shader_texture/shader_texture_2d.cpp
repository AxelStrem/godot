/**************************************************************************/
/*  shader_texture_2d.cpp                                                */
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

#include "shader_texture_2d.h"

#include "core/object/callable_method_pointer.h"
#include "core/templates/list.h"
#include "core/templates/local_vector.h"
#include "core/templates/pair.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

ShaderTexture2D::ShaderTexture2D() {
	_queue_update();
}

ShaderTexture2D::~ShaderTexture2D() {
	_clear_parameter_dependencies();
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	if (texture.is_valid()) {
		RS::get_singleton()->free_rid(texture);
	}
	if (shader.is_valid()) {
		shader->disconnect_changed(callable_mp(this, &ShaderTexture2D::_shader_changed));
	}
}

void ShaderTexture2D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_width", "width"), &ShaderTexture2D::set_width);

	ClassDB::bind_method(D_METHOD("set_height", "height"), &ShaderTexture2D::set_height);

	ClassDB::bind_method(D_METHOD("set_generate_mipmaps", "enable"), &ShaderTexture2D::set_generate_mipmaps);
	ClassDB::bind_method(D_METHOD("is_generating_mipmaps"), &ShaderTexture2D::is_generating_mipmaps);

	ClassDB::bind_method(D_METHOD("set_channels", "layout"), &ShaderTexture2D::set_channels);
	ClassDB::bind_method(D_METHOD("get_channels"), &ShaderTexture2D::get_channels);

	ClassDB::bind_method(D_METHOD("set_precision", "precision"), &ShaderTexture2D::set_precision);
	ClassDB::bind_method(D_METHOD("get_precision"), &ShaderTexture2D::get_precision);

	ClassDB::bind_method(D_METHOD("set_shader", "shader"), &ShaderTexture2D::set_shader);
	ClassDB::bind_method(D_METHOD("get_shader"), &ShaderTexture2D::get_shader);
	ClassDB::bind_method(D_METHOD("set_shader_parameter", "param", "value"), &ShaderTexture2D::set_shader_parameter);
	ClassDB::bind_method(D_METHOD("get_shader_parameter", "param"), &ShaderTexture2D::get_shader_parameter);

	ClassDB::bind_method(D_METHOD("set_shader_parameters", "parameters"), &ShaderTexture2D::set_shader_parameters);
	ClassDB::bind_method(D_METHOD("get_shader_parameters"), &ShaderTexture2D::get_shader_parameters);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "width", PROPERTY_HINT_RANGE, "1,16384,1,or_greater,suffix:px"), "set_width", "get_width");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "height", PROPERTY_HINT_RANGE, "1,16384,1,or_greater,suffix:px"), "set_height", "get_height");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "generate_mipmaps"), "set_generate_mipmaps", "is_generating_mipmaps");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "channels", PROPERTY_HINT_ENUM, "L,RG,RGB,RGBA"), "set_channels", "get_channels");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "precision", PROPERTY_HINT_ENUM, "8-bit,16-bit,Half,Float"), "set_precision", "get_precision");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "shader", PROPERTY_HINT_RESOURCE_TYPE, "Shader"), "set_shader", "get_shader");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "shader_parameters"), "set_shader_parameters", "get_shader_parameters");

	BIND_ENUM_CONSTANT(CHANNEL_L);
	BIND_ENUM_CONSTANT(CHANNEL_RG);
	BIND_ENUM_CONSTANT(CHANNEL_RGB);
	BIND_ENUM_CONSTANT(CHANNEL_RGBA);

	BIND_ENUM_CONSTANT(PRECISION_8);
	BIND_ENUM_CONSTANT(PRECISION_16);
	BIND_ENUM_CONSTANT(PRECISION_HALF);
	BIND_ENUM_CONSTANT(PRECISION_FLOAT);
}

bool ShaderTexture2D::_set(const StringName &p_name, const Variant &p_value) {
	if (shader.is_valid()) {
		if (const StringName *uniform_name = remap_cache.getptr(p_name)) {
			set_shader_parameter(*uniform_name, p_value);
			return true;
		}

		String property_name = p_name;
		if (property_name.begins_with("shader_parameter/")) {
			String param = property_name.replace_first("shader_parameter/", "");
			StringName uniform = param;
			remap_cache.insert(p_name, uniform);
			set_shader_parameter(uniform, p_value);
			return true;
		}
	}

	return false;
}

bool ShaderTexture2D::_get(const StringName &p_name, Variant &r_ret) const {
	if (shader.is_valid()) {
		if (const StringName *uniform_name = remap_cache.getptr(p_name)) {
			r_ret = get_shader_parameter(*uniform_name);
			return true;
		}
	}

	return false;
}

void ShaderTexture2D::_get_property_list(List<PropertyInfo> *p_list) const {
	Texture2D::_get_property_list(p_list);

	if (shader.is_null()) {
		return;
	}

	List<PropertyInfo> uniform_list;
	shader->get_shader_uniform_list(&uniform_list, true);
	ShaderTexture2D *self = const_cast<ShaderTexture2D *>(this);

	HashMap<String, HashMap<String, List<PropertyInfo>>> groups;
	LocalVector<Pair<String, LocalVector<String>>> vgroups;
	{
		HashMap<String, List<PropertyInfo>> none_subgroup;
		none_subgroup.insert("<None>", List<PropertyInfo>());
		groups.insert("<None>", none_subgroup);
	}

	String last_group = "<None>";
	String last_subgroup = "<None>";

	bool is_none_group_undefined = true;
	bool is_none_group = true;

	for (const PropertyInfo &pi : uniform_list) {
		if (pi.usage == PROPERTY_USAGE_GROUP) {
			if (!pi.name.is_empty()) {
				Vector<String> vgroup = pi.name.split("::");
				last_group = vgroup[0];
				if (vgroup.size() > 1) {
					last_subgroup = vgroup[1];
				} else {
					last_subgroup = "<None>";
				}
				is_none_group = false;

				if (!groups.has(last_group)) {
					PropertyInfo info;
					info.usage = PROPERTY_USAGE_GROUP;
					info.name = last_group.capitalize();
					info.hint_string = "shader_parameter/";

					List<PropertyInfo> none_subgroup;
					none_subgroup.push_back(info);

					HashMap<String, List<PropertyInfo>> subgroup_map;
					subgroup_map.insert("<None>", none_subgroup);

					groups.insert(last_group, subgroup_map);
					vgroups.push_back(Pair<String, LocalVector<String>>(last_group, { "<None>" }));
				}

				if (!groups[last_group].has(last_subgroup)) {
					PropertyInfo info;
					info.usage = PROPERTY_USAGE_SUBGROUP;
					info.name = last_subgroup.capitalize();
					info.hint_string = "shader_parameter/";

					List<PropertyInfo> subgroup;
					subgroup.push_back(info);

					groups[last_group].insert(last_subgroup, subgroup);
					for (Pair<String, LocalVector<String>> &group : vgroups) {
						if (group.first == last_group) {
							group.second.push_back(last_subgroup);
							break;
						}
					}
				}
			} else {
				last_group = "<None>";
				last_subgroup = "<None>";
				is_none_group = true;
			}
			continue;
		}

		if (is_none_group_undefined && is_none_group) {
			is_none_group_undefined = false;

			PropertyInfo info;
			info.usage = PROPERTY_USAGE_GROUP;
			info.name = "Shader Parameters";
			info.hint_string = "shader_parameter/";
			groups["<None>"]["<None>"].push_back(info);

			vgroups.push_back(Pair<String, LocalVector<String>>("<None>", { "<None>" }));
		}

		Variant *cached = param_cache.getptr(pi.name);
		if (!cached) {
			const Variant *dict_value = shader_parameters.getptr(pi.name);
			if (dict_value) {
				param_cache.insert(pi.name, *dict_value);
				cached = param_cache.getptr(pi.name);
			}
		}

		bool is_uniform_cached = cached != nullptr;
		bool is_uniform_type_compatible = true;

		if (is_uniform_cached) {
			Variant::Type cached_type = cached->get_type();

			if (cached->is_array()) {
				is_uniform_type_compatible = Variant::can_convert(pi.type, cached_type);
			} else {
				is_uniform_type_compatible = pi.type == cached_type;
			}

#ifndef DISABLE_DEPRECATED
			if (!is_uniform_type_compatible && pi.type == Variant::PACKED_VECTOR4_ARRAY && cached_type == Variant::PACKED_FLOAT32_ARRAY) {
				PackedVector4Array varray;
				PackedFloat32Array array = (PackedFloat32Array)*cached;

				for (int i = 0; i + 3 < array.size(); i += 4) {
					varray.push_back(Vector4(array[i], array[i + 1], array[i + 2], array[i + 3]));
				}

				param_cache.insert(pi.name, varray);
				cached = param_cache.getptr(pi.name);
				cached_type = cached ? cached->get_type() : Variant::NIL;
				is_uniform_type_compatible = true;
			}
#endif

			if (is_uniform_type_compatible && pi.type == Variant::OBJECT && cached && cached_type == Variant::OBJECT) {
				Object *cached_obj = *cached;
				if (cached_obj && !cached_obj->is_class(pi.hint_string)) {
					is_uniform_type_compatible = false;
				}
			}
		}

		PropertyInfo info = pi;
		info.name = "shader_parameter/" + info.name;

		if (!is_uniform_cached || !is_uniform_type_compatible) {
			Variant default_value = RenderingServer::get_singleton()->shader_get_parameter_default(shader->get_rid(), pi.name);
			param_cache.insert(pi.name, default_value);
			if (shader_parameters.has(pi.name)) {
				shader_parameters.set(pi.name, default_value);
				self->_update_parameter_dependency(pi.name, default_value);
			}
		}

		remap_cache.insert(info.name, pi.name);
		groups[last_group][last_subgroup].push_back(info);
	}

	for (const Pair<String, LocalVector<String>> &group_pair : vgroups) {
		const String &group_name = group_pair.first;
		for (const String &subgroup_name : group_pair.second) {
			List<PropertyInfo> &prop_infos = groups[group_name][subgroup_name];
			for (const PropertyInfo &item : prop_infos) {
				p_list->push_back(item);
			}
		}
	}
}

bool ShaderTexture2D::_property_can_revert(const StringName &p_name) const {
	if (shader.is_valid() && remap_cache.has(p_name)) {
		return true;
	}
	return false;
}

bool ShaderTexture2D::_property_get_revert(const StringName &p_name, Variant &r_property) const {
	if (shader.is_valid()) {
		if (const StringName *uniform_name = remap_cache.getptr(p_name)) {
			r_property = RenderingServer::get_singleton()->shader_get_parameter_default(shader->get_rid(), *uniform_name);
			return true;
		}
	}
	return false;
}

void ShaderTexture2D::_validate_property(PropertyInfo &p_property) const {
	Texture2D::_validate_property(p_property);
	if (p_property.name == "shader_parameters") {
		p_property.usage = PROPERTY_USAGE_STORAGE;
	}
}

void ShaderTexture2D::_shader_changed() {
	remap_cache.clear();
	_rebuild_parameter_dependencies();
	notify_property_list_changed();
	_queue_update();
}

void ShaderTexture2D::_update_parameter_dependency(const StringName &p_param, const Variant &p_value) {
	_remove_parameter_dependency(p_param);
	if (p_value.get_type() != Variant::OBJECT) {
		return;
	}

	Ref<Resource> resource = p_value;
	if (resource.is_null()) {
		return;
	}

	ObjectID id = resource->get_instance_id();
	parameter_resource_map.insert(p_param, id);
	int *count = resource_connection_counts.getptr(id);
	if (count) {
		(*count)++;
	} else {
		resource_connection_counts.insert(id, 1);
		resource->connect_changed(callable_mp(this, &ShaderTexture2D::_dependency_resource_changed));
	}
}

void ShaderTexture2D::_remove_parameter_dependency(const StringName &p_param) {
	ObjectID *id_ptr = parameter_resource_map.getptr(p_param);
	if (!id_ptr) {
		return;
	}

	ObjectID id = *id_ptr;
	parameter_resource_map.erase(p_param);
	int *count = resource_connection_counts.getptr(id);
	if (!count) {
		return;
	}
	(*count)--;
	if (*count > 0) {
		return;
	}

	resource_connection_counts.erase(id);
	Object *obj = ObjectDB::get_instance(id);
	Resource *resource = Object::cast_to<Resource>(obj);
	if (resource) {
		resource->disconnect_changed(callable_mp(this, &ShaderTexture2D::_dependency_resource_changed));
	}
}

void ShaderTexture2D::_clear_parameter_dependencies() {
	if (resource_connection_counts.is_empty()) {
		parameter_resource_map.clear();
		return;
	}

	Callable callable = callable_mp(this, &ShaderTexture2D::_dependency_resource_changed);
	for (const KeyValue<ObjectID, int> &E : resource_connection_counts) {
		if (E.value <= 0) {
			continue;
		}
		Object *obj = ObjectDB::get_instance(E.key);
		Resource *resource = Object::cast_to<Resource>(obj);
		if (resource) {
			resource->disconnect_changed(callable);
		}
	}

	resource_connection_counts.clear();
	parameter_resource_map.clear();
}

void ShaderTexture2D::_rebuild_parameter_dependencies() {
	_clear_parameter_dependencies();
	Array keys = shader_parameters.keys();
	for (int i = 0; i < keys.size(); i++) {
		Variant key = keys[i];
		StringName param_name = StringName(key);
		Variant value = shader_parameters.get(key, Variant());
		_update_parameter_dependency(param_name, value);
	}
}

void ShaderTexture2D::_dependency_resource_changed() {
	_queue_update();
}

void ShaderTexture2D::_queue_update() {
	if (update_queued) {
		return;
	}
	update_queued = true;
	callable_mp(this, &ShaderTexture2D::_update_texture).call_deferred();
}

void ShaderTexture2D::_update_texture() {
	update_queued = false;

	Ref<Image> generated = _generate_image();
	if (generated.is_null() || generated->is_empty()) {
		return;
	}

	Image::Format target_format = _get_target_format();
	if (generated->get_format() != target_format) {
		generated->convert(target_format);
	}

	if (generate_mipmaps && !generated->has_mipmaps()) {
		generated->generate_mipmaps();
	}

	_set_texture_image(generated);
}

void ShaderTexture2D::_set_texture_image(const Ref<Image> &p_image) {
	image = p_image;
	if (image.is_null()) {
		return;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rs);

	if (texture.is_valid()) {
		RID new_texture = rs->texture_2d_create(image);
		rs->texture_replace(texture, new_texture);
	} else {
		texture = rs->texture_2d_create(image);
	}
	RS::get_singleton()->texture_set_path(texture, get_path());
	emit_changed();
}

Ref<Image> ShaderTexture2D::_generate_image() {
	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rs, Ref<Image>());

	if (shader.is_null()) {
		return image;
	}

	RID viewport = rs->viewport_create();
	rs->viewport_set_size(viewport, size.x, size.y);
	rs->viewport_set_update_mode(viewport, RS::VIEWPORT_UPDATE_ONCE);
	rs->viewport_set_use_hdr_2d(viewport, _needs_hdr_buffer());
	rs->viewport_set_use_hdr_2d_full_precision(viewport, precision == PRECISION_FLOAT);
	rs->viewport_set_transparent_background(viewport, true);
	rs->viewport_set_active(viewport, true);

	RID canvas = rs->canvas_create();
	rs->viewport_attach_canvas(viewport, canvas);

	RID canvas_item = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item, canvas);

	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader);

	Array keys = shader_parameters.keys();
	for (int i = 0; i < keys.size(); i++) {
		Variant key = keys[i];
		StringName param_name = StringName(key);
		Variant value = shader_parameters.get(key, Variant());
		material->set_shader_parameter(param_name, value);
	}

	rs->canvas_item_set_material(canvas_item, material->get_rid());
	rs->canvas_item_add_rect(canvas_item, Rect2(Vector2(), Vector2((real_t)size.x, (real_t)size.y)), Color(1, 1, 1, 1));

	rs->sync();
	rs->draw(false);
	rs->sync();

	RID viewport_texture = rs->viewport_get_texture(viewport);
	Ref<Image> rendered = rs->texture_2d_get(viewport_texture);

	rs->free_rid(canvas_item);
	rs->free_rid(canvas);
	rs->free_rid(viewport);

	return rendered;
}

Image::Format ShaderTexture2D::_get_target_format() const {
	switch (channels) {
		case CHANNEL_L: {
			switch (precision) {
				case PRECISION_8:
					return Image::FORMAT_L8;
				case PRECISION_16:
					return Image::FORMAT_L16;
				case PRECISION_HALF:
					return Image::FORMAT_LH;
				case PRECISION_FLOAT:
					return Image::FORMAT_LF;
			}
		} break;
		case CHANNEL_RG: {
			switch (precision) {
				case PRECISION_8:
					return Image::FORMAT_RG8;
				case PRECISION_16:
					return Image::FORMAT_RG16;
				case PRECISION_HALF:
					return Image::FORMAT_RGH;
				case PRECISION_FLOAT:
					return Image::FORMAT_RGF;
			}
		} break;
		case CHANNEL_RGB: {
			switch (precision) {
				case PRECISION_8:
					return Image::FORMAT_RGB8;
				case PRECISION_16:
					return Image::FORMAT_RGB16;
				case PRECISION_HALF:
					return Image::FORMAT_RGBH;
				case PRECISION_FLOAT:
					return Image::FORMAT_RGBF;
			}
		} break;
		case CHANNEL_RGBA: {
			switch (precision) {
				case PRECISION_8:
					return Image::FORMAT_RGBA8;
				case PRECISION_16:
					return Image::FORMAT_RGBA16;
				case PRECISION_HALF:
					return Image::FORMAT_RGBAH;
				case PRECISION_FLOAT:
					return Image::FORMAT_RGBAF;
			}
		} break;
	}

	return Image::FORMAT_RGBA8;
}

bool ShaderTexture2D::_needs_hdr_buffer() const {
	return precision == PRECISION_16 || precision == PRECISION_HALF || precision == PRECISION_FLOAT;
}

void ShaderTexture2D::set_width(int p_width) {
	ERR_FAIL_COND(p_width <= 0);
	if (size.x == p_width) {
		return;
	}
	size.x = p_width;
	_queue_update();
}

int ShaderTexture2D::get_width() const {
	return size.x;
}

void ShaderTexture2D::set_height(int p_height) {
	ERR_FAIL_COND(p_height <= 0);
	if (size.y == p_height) {
		return;
	}
	size.y = p_height;
	_queue_update();
}

int ShaderTexture2D::get_height() const {
	return size.y;
}

void ShaderTexture2D::set_generate_mipmaps(bool p_enable) {
	if (generate_mipmaps == p_enable) {
		return;
	}
	generate_mipmaps = p_enable;
	_queue_update();
}

bool ShaderTexture2D::is_generating_mipmaps() const {
	return generate_mipmaps;
}

void ShaderTexture2D::set_channels(ChannelLayout p_layout) {
	if (channels == p_layout) {
		return;
	}
	channels = p_layout;
	_queue_update();
}

ShaderTexture2D::ChannelLayout ShaderTexture2D::get_channels() const {
	return channels;
}

void ShaderTexture2D::set_precision(Precision p_precision) {
	if (precision == p_precision) {
		return;
	}
	precision = p_precision;
	_queue_update();
}

ShaderTexture2D::Precision ShaderTexture2D::get_precision() const {
	return precision;
}

void ShaderTexture2D::set_shader(const Ref<Shader> &p_shader) {
	if (shader == p_shader) {
		return;
	}
	if (shader.is_valid()) {
		shader->disconnect_changed(callable_mp(this, &ShaderTexture2D::_shader_changed));
	}
	shader = p_shader;
	if (shader.is_valid()) {
		shader->connect_changed(callable_mp(this, &ShaderTexture2D::_shader_changed));
	}
	_shader_changed();
}

Ref<Shader> ShaderTexture2D::get_shader() const {
	return shader;
}

void ShaderTexture2D::set_shader_parameter(const StringName &p_param, const Variant &p_value) {
	_remove_parameter_dependency(p_param);
	if (p_value.get_type() == Variant::NIL) {
		param_cache.erase(p_param);
		if (shader_parameters.has(p_param)) {
			shader_parameters.erase(p_param);
		}
	} else {
		Variant *cached = param_cache.getptr(p_param);
		if (cached) {
			*cached = p_value;
		} else {
			param_cache.insert(p_param, p_value);
		}
		shader_parameters.set(p_param, p_value);
		_update_parameter_dependency(p_param, p_value);
	}

	StringName property_name = StringName("shader_parameter/" + String(p_param));
	remap_cache.insert(property_name, p_param);
	_queue_update();
}

Variant ShaderTexture2D::get_shader_parameter(const StringName &p_param) const {
	if (const Variant *cached = param_cache.getptr(p_param)) {
		return *cached;
	}
	return shader_parameters.get(p_param, Variant());
}

void ShaderTexture2D::set_shader_parameters(const Dictionary &p_parameters) {
	_clear_parameter_dependencies();
	shader_parameters.clear();
	param_cache.clear();
	Array keys = p_parameters.keys();
	for (int i = 0; i < keys.size(); i++) {
		Variant key = keys[i];
		StringName param_name = StringName(key);
		Variant value = p_parameters.get(key, Variant());
		shader_parameters.set(param_name, value);
		param_cache.insert(param_name, value);
		StringName property_name = StringName("shader_parameter/" + String(param_name));
		remap_cache.insert(property_name, param_name);
		_update_parameter_dependency(param_name, value);
	}
	_queue_update();
}

Dictionary ShaderTexture2D::get_shader_parameters() const {
	return shader_parameters;
}

RID ShaderTexture2D::get_rid() const {
	if (!texture.is_valid()) {
		texture = RS::get_singleton()->texture_2d_placeholder_create();
	}
	return texture;
}

bool ShaderTexture2D::has_alpha() const {
	return channels == CHANNEL_RGBA;
}

Ref<Image> ShaderTexture2D::get_image() const {
	return image;
}
