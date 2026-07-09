/**************************************************************************/
/*  tentacle_renderer.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/math/vector3.h"
#include "core/math/vector2i.h"
#include "core/math/transform_3d.h"
#include "core/math/projection.h"
#include "core/templates/vector.h"
#include "servers/rendering/rendering_device.h"

class TentacleRenderer : public Object {
	GDCLASS(TentacleRenderer, Object);

public:
	// ---- Tentacle instance layout (std430-compatible, 48 bytes) ----
	// Mirrors the GLSL Tentacle struct in tentacle.glsl.
	struct TentacleInstance {
		Vector3 start = Vector3();
		float _pad0 = 0.0f; // 4 bytes
		Vector3 end = Vector3();
		float _pad1 = 0.0f; // 4 bytes
		float progress = 0.0f; // 0→1 growth
		float thickness = 0.1f; // base width
		float _pad2 = 0.0f; // 8 bytes trailing padding
		float _pad3 = 0.0f; // (48 bytes total, 16-byte aligned)
	};

private:
	// ---- Configuration ----
	uint32_t _max_tentacles = 512;
	uint32_t _segments = 64;
	Vector2i _size = Vector2i(1920, 1080);

	// ---- Wiggle / width parameters ----
	float _noise_amplitude = 0.4f;
	float _noise_frequency = 0.8f;
	float _noise_speed = 0.3f;
	float _taper_amount = 0.7f;
	float _width_scale = 0.05f;

	// ---- State ----
	RenderingDevice *_rd = nullptr;
	bool _initialized = false;

	// ---- Tentacle data (CPU mirror, then uploaded to GPU) ----
	Vector<TentacleInstance> _instances;
	int _active_count = 0;

	// ---- RD resources ----
	RID _shader;
	RID _pipeline;
	RID _color_texture;
	RID _depth_texture;
	RID _framebuffer;
	RID _instance_buffer; // Storage buffer: TentacleInstance[]
	RID _uniform_set;

	RenderingDevice::FramebufferFormatID _fb_format = RenderingDevice::INVALID_FORMAT_ID;

	// ---- Internal helpers ----
	void _create_textures();
	void _create_framebuffer();
	void _create_shader_and_pipeline();
	void _create_instance_buffer();
	void _create_uniform_set();
	void _resize_textures();
	void _cleanup_rd_resources();
	void _ensure_rd();

	uint32_t _instance_size_bytes() const;
	uint32_t _buffer_capacity_bytes() const;

protected:
	static void _bind_methods();

public:
	// ---- Setup ----
	void initialize();
	void set_size(const Vector2i &p_size);
	void set_max_tentacles(uint32_t p_count);
	void set_segments(uint32_t p_segments);
	void set_noise_amplitude(float p_val);
	void set_noise_frequency(float p_val);
	void set_noise_speed(float p_val);
	void set_taper_amount(float p_val);
	void set_width_scale(float p_val);

	// ---- Per-frame data upload ----
	void set_tentacle_data(const PackedFloat32Array &p_data, int p_count);
	void clear_tentacles();

	// ---- Render ----
	RID render(const Transform3D &p_camera_transform, const Projection &p_camera_projection, float p_time);

	// ---- Access ----
	RID get_color_texture() const { return _color_texture; }
	PackedByteArray get_color_texture_data() const;
	bool is_initialized() const { return _initialized; }
	Vector2i get_size() const { return _size; }
	int get_active_count() const { return _active_count; }

	TentacleRenderer();
	~TentacleRenderer();
};
