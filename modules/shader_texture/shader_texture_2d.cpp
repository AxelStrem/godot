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
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

ShaderTexture2D::ShaderTexture2D() {
	_queue_update();
}

ShaderTexture2D::~ShaderTexture2D() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	if (texture.is_valid()) {
		RS::get_singleton()->free_rid(texture);
	}
	if (shader.is_valid()) {
		shader->disconnect_changed(callable_mp(this, &ShaderTexture2D::_queue_update));
	}
}

void ShaderTexture2D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_width", "width"), &ShaderTexture2D::set_width);
	ClassDB::bind_method(D_METHOD("get_width"), &ShaderTexture2D::get_width);

	ClassDB::bind_method(D_METHOD("set_height", "height"), &ShaderTexture2D::set_height);
	ClassDB::bind_method(D_METHOD("get_height"), &ShaderTexture2D::get_height);

	ClassDB::bind_method(D_METHOD("set_generate_mipmaps", "enable"), &ShaderTexture2D::set_generate_mipmaps);
	ClassDB::bind_method(D_METHOD("is_generating_mipmaps"), &ShaderTexture2D::is_generating_mipmaps);

	ClassDB::bind_method(D_METHOD("set_channels", "layout"), &ShaderTexture2D::set_channels);
	ClassDB::bind_method(D_METHOD("get_channels"), &ShaderTexture2D::get_channels);

	ClassDB::bind_method(D_METHOD("set_precision", "precision"), &ShaderTexture2D::set_precision);
	ClassDB::bind_method(D_METHOD("get_precision"), &ShaderTexture2D::get_precision);

	ClassDB::bind_method(D_METHOD("set_shader", "shader"), &ShaderTexture2D::set_shader);
	ClassDB::bind_method(D_METHOD("get_shader"), &ShaderTexture2D::get_shader);

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
		StringName param_name = StringName(keys[i]);
		material->set_shader_parameter(param_name, shader_parameters[keys[i]]);
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
	return precision == PRECISION_HALF || precision == PRECISION_FLOAT;
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
		shader->disconnect_changed(callable_mp(this, &ShaderTexture2D::_queue_update));
	}
	shader = p_shader;
	if (shader.is_valid()) {
		shader->connect_changed(callable_mp(this, &ShaderTexture2D::_queue_update));
	}
	_queue_update();
}

Ref<Shader> ShaderTexture2D::get_shader() const {
	return shader;
}

void ShaderTexture2D::set_shader_parameters(const Dictionary &p_parameters) {
	shader_parameters = p_parameters;
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
