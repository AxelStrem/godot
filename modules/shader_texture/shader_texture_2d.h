/**************************************************************************/
/*  shader_texture_2d.h                                                  */
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

#include "core/object/object_id.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/list.h"
#include "scene/resources/shader.h"
#include "scene/resources/texture.h"

class ShaderTexture2D : public Texture2D {
	GDCLASS(ShaderTexture2D, Texture2D);

public:
	enum ChannelLayout {
		CHANNEL_L,
		CHANNEL_RG,
		CHANNEL_RGB,
		CHANNEL_RGBA,
	};

	enum Precision {
		PRECISION_8,
		PRECISION_16,
		PRECISION_HALF,
		PRECISION_FLOAT,
	};

private:
	Ref<Image> image;
	mutable RID texture;

	Size2i size = Size2i(512, 512);
	bool generate_mipmaps = false;
	ChannelLayout channels = CHANNEL_RGBA;
	Precision precision = PRECISION_8;

	Ref<Shader> shader;
	mutable Dictionary shader_parameters;
	mutable HashMap<StringName, Variant> param_cache;
	mutable HashMap<StringName, StringName> remap_cache;
	mutable HashMap<StringName, ObjectID> parameter_resource_map;
	mutable HashMap<ObjectID, int> resource_connection_counts;

	bool update_queued = false;

	void _queue_update();
	void _update_texture();
	Ref<Image> _generate_image();
	void _set_texture_image(const Ref<Image> &p_image);
	void _shader_changed();
	void _update_parameter_dependency(const StringName &p_param, const Variant &p_value);
	void _remove_parameter_dependency(const StringName &p_param);
	void _clear_parameter_dependencies();
	void _rebuild_parameter_dependencies();
	void _dependency_resource_changed();

	Image::Format _get_target_format() const;
	bool _needs_hdr_buffer() const;

protected:
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	bool _property_can_revert(const StringName &p_name) const;
	bool _property_get_revert(const StringName &p_name, Variant &r_property) const;
	void _validate_property(PropertyInfo &p_property) const;

protected:
	static void _bind_methods();

public:
	void set_width(int p_width);
	int get_width() const override;

	void set_height(int p_height);
	int get_height() const override;

	void set_generate_mipmaps(bool p_enable);
	bool is_generating_mipmaps() const;

	void set_channels(ChannelLayout p_layout);
	ChannelLayout get_channels() const;

	void set_precision(Precision p_precision);
	Precision get_precision() const;

	void set_shader(const Ref<Shader> &p_shader);
	Ref<Shader> get_shader() const;

	void set_shader_parameter(const StringName &p_param, const Variant &p_value);
	Variant get_shader_parameter(const StringName &p_param) const;

	void set_shader_parameters(const Dictionary &p_parameters);
	Dictionary get_shader_parameters() const;

	virtual RID get_rid() const override;
	virtual bool has_alpha() const override;
	virtual Ref<Image> get_image() const override;

	ShaderTexture2D();
	~ShaderTexture2D();
};

VARIANT_ENUM_CAST(ShaderTexture2D::ChannelLayout);
VARIANT_ENUM_CAST(ShaderTexture2D::Precision);
