/**************************************************************************/
/*  tentacle_renderer.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "tentacle_renderer.h"

#include "core/math/projection.h"
#include "core/object/class_db.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/shader_compiler.h"

// Generated from tentacle.glsl by the build system.
#include "tentacle.glsl.gen.h"

// ============================================================================
// Push-constant layout (must match the shader's PushConstants block).
// Total: 112 bytes (fits in 128-byte min-guaranteed push-constant limit).
// ============================================================================
struct PushConstant {
	uint32_t segments;
	uint32_t tentacle_count;
	float time;
	float noise_amplitude;
	float noise_freq;
	float noise_speed;
	float taper_amount;
	float width_scale;
	float view_proj[16]; // mat4, column-major
	float cam_pos[4]; // vec3 + 4-byte pad
};

static_assert(sizeof(PushConstant) == 112, "PushConstant size mismatch with shader");

// ============================================================================
// Helpers
// ============================================================================

uint32_t TentacleRenderer::_instance_size_bytes() const {
	return sizeof(TentacleInstance); // 48
}

uint32_t TentacleRenderer::_buffer_capacity_bytes() const {
	return _max_tentacles * _instance_size_bytes();
}

void TentacleRenderer::_cleanup_rd_resources() {
	if (!_rd) {
		return;
	}

	if (_framebuffer.is_valid()) {
		_rd->free_rid(_framebuffer);
	}
	if (_color_texture.is_valid()) {
		_rd->free_rid(_color_texture);
	}
	if (_depth_texture.is_valid()) {
		_rd->free_rid(_depth_texture);
	}
	if (_pipeline.is_valid()) {
		_rd->free_rid(_pipeline);
	}
	if (_shader.is_valid()) {
		_rd->free_rid(_shader);
	}
	if (_instance_buffer.is_valid()) {
		_rd->free_rid(_instance_buffer);
	}
	// Uniform sets are freed automatically when shader is freed.

	_framebuffer = RID();
	_color_texture = RID();
	_depth_texture = RID();
	_pipeline = RID();
	_shader = RID();
	_instance_buffer = RID();
	_uniform_set = RID();
	_fb_format = RenderingDevice::INVALID_FORMAT_ID;

	if (_rd) {
		memdelete(_rd);
		_rd = nullptr;
	}
}

void TentacleRenderer::_ensure_rd() {
	if (_rd) {
		return;
	}

	_rd = RenderingServer::get_singleton()->create_local_rendering_device();
	ERR_FAIL_NULL_MSG(_rd, "TentacleRenderer: Failed to create local RenderingDevice. "
						  "Ensure the project uses the Forward+ or Mobile renderer.");
}

// ============================================================================
// Texture / framebuffer creation
// ============================================================================

void TentacleRenderer::_create_textures() {
	ERR_FAIL_NULL(_rd);

	// Free old textures if they exist.
	if (_color_texture.is_valid()) {
		_rd->free_rid(_color_texture);
	}
	if (_depth_texture.is_valid()) {
		_rd->free_rid(_depth_texture);
	}

	// ---- Color texture ----
	{
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		tf.width = _size.x;
		tf.height = _size.y;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
		tf.texture_type = RD::TEXTURE_TYPE_2D;

		RD::TextureView tv;
		_color_texture = _rd->texture_create(tf, tv);
	}

	// ---- Depth texture ----
	{
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_D32_SFLOAT;
		tf.width = _size.x;
		tf.height = _size.y;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		tf.texture_type = RD::TEXTURE_TYPE_2D;

		RD::TextureView tv;
		_depth_texture = _rd->texture_create(tf, tv);
	}
}

void TentacleRenderer::_create_framebuffer() {
	ERR_FAIL_NULL(_rd);

	if (_framebuffer.is_valid()) {
		_rd->free_rid(_framebuffer);
	}

	Vector<RID> attachments;
	attachments.push_back(_color_texture);
	attachments.push_back(_depth_texture);

	_framebuffer = _rd->framebuffer_create(attachments);
	_fb_format = _rd->framebuffer_get_format(_framebuffer);
}

void TentacleRenderer::_resize_textures() {
	_cleanup_rd_resources();
	_ensure_rd();
	_create_textures();
	_create_framebuffer();
	_create_shader_and_pipeline();
	_create_instance_buffer();
	_create_uniform_set();
}

// ============================================================================
// Shader & pipeline
// ============================================================================

void TentacleRenderer::_create_shader_and_pipeline() {
	ERR_FAIL_NULL(_rd);

	if (_shader.is_valid()) {
		_rd->free_rid(_shader);
	}

	// Compile the GLSL source to SPIR-V.
	Ref<RDShaderFile> shader_file;
	shader_file.instantiate();

	Error err = shader_file->parse_versions_from_text(tentacle_shader_glsl);
	ERR_FAIL_COND_MSG(err != OK, "TentacleRenderer: Failed to parse shader source.");

	Vector<RD::ShaderStageSPIRVData> spirv_stages = shader_file->get_spirv_stages();
	ERR_FAIL_COND_MSG(spirv_stages.is_empty(), "TentacleRenderer: No SPIR-V stages compiled.");

	_shader = _rd->shader_create_from_spirv(spirv_stages);
	ERR_FAIL_COND_MSG(!_shader.is_valid(), "TentacleRenderer: Failed to create shader from SPIR-V.");

	// Pipeline: procedural geometry (no vertex buffer), triangle strip.
	RD::PipelineRasterizationState rs;
	// Explicit defaults — no wireframe, standard cull.

	RD::PipelineMultisampleState ms;
	ms.sample_count = RD::TEXTURE_SAMPLES_1;

	RD::PipelineDepthStencilState ds;
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_LESS;

	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_blend(1);

	_pipeline = _rd->render_pipeline_create(
			_shader,
			_fb_format,
			RenderingDevice::INVALID_FORMAT_ID, // procedural, no vertex buffer
			RD::RENDER_PRIMITIVE_TRIANGLE_STRIPS,
			rs,
			ms,
			ds,
			bs,
			0 // no dynamic state flags
	);

	ERR_FAIL_COND_MSG(!_pipeline.is_valid(), "TentacleRenderer: Failed to create render pipeline.");
}

// ============================================================================
// Instance buffer (storage buffer for tentacle data)
// ============================================================================

void TentacleRenderer::_create_instance_buffer() {
	ERR_FAIL_NULL(_rd);

	if (_instance_buffer.is_valid()) {
		_rd->free_rid(_instance_buffer);
	}

	Vector<uint8_t> empty_data;
	empty_data.resize(_buffer_capacity_bytes());
	memset(empty_data.ptrw(), 0, empty_data.size());

	_instance_buffer = _rd->storage_buffer_create(empty_data.size(), empty_data);
}

// ============================================================================
// Uniform set
// ============================================================================

void TentacleRenderer::_create_uniform_set() {
	ERR_FAIL_NULL(_rd);

	Vector<RD::Uniform> uniforms;

	// Binding 0: storage buffer of TentacleInstance[].
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 0;
		u.append_id(_instance_buffer);
		uniforms.push_back(u);
	}

	_uniform_set = _rd->uniform_set_create(uniforms, _shader, 0);
	ERR_FAIL_COND_MSG(!_uniform_set.is_valid(), "TentacleRenderer: Failed to create uniform set.");
}

// ============================================================================
// Public API
// ============================================================================

void TentacleRenderer::initialize() {
	if (_initialized) {
		return;
	}

	_ensure_rd();
	_create_textures();
	_create_framebuffer();
	_create_shader_and_pipeline();
	_create_instance_buffer();
	_create_uniform_set();

	_instances.resize(_max_tentacles);
	_active_count = 0;

	_initialized = true;
}

void TentacleRenderer::set_size(const Vector2i &p_size) {
	if (_size == p_size && _initialized) {
		return;
	}
	_size = p_size;
	if (_initialized) {
		_resize_textures();
	}
}

void TentacleRenderer::set_max_tentacles(uint32_t p_count) {
	_max_tentacles = MAX(p_count, 1u);
	if (_initialized) {
		_instances.resize(_max_tentacles);
		_create_instance_buffer();
		_create_uniform_set();
	}
}

void TentacleRenderer::set_segments(uint32_t p_segments) {
	_segments = CLAMP(p_segments, 4u, 256u);
}

void TentacleRenderer::set_noise_amplitude(float p_val) { _noise_amplitude = p_val; }
void TentacleRenderer::set_noise_frequency(float p_val) { _noise_frequency = p_val; }
void TentacleRenderer::set_noise_speed(float p_val) { _noise_speed = p_val; }
void TentacleRenderer::set_taper_amount(float p_val) { _taper_amount = p_val; }
void TentacleRenderer::set_width_scale(float p_val) { _width_scale = p_val; }

void TentacleRenderer::set_tentacle_data(const PackedFloat32Array &p_data, int p_count) {
	ERR_FAIL_COND(!_initialized);

	_active_count = CLAMP(p_count, 0, int(_max_tentacles));
	int expected_floats = _active_count * 12; // 12 floats per instance (48 bytes / 4)
	ERR_FAIL_COND_MSG(p_data.size() < expected_floats,
			vformat("TentacleRenderer: Not enough data. Expected %d floats, got %d.",
					expected_floats, p_data.size()));

	const float *src = p_data.ptr();
	TentacleInstance *dst = _instances.ptrw();

	// Copy directly — the PackedFloat32Array layout matches our struct layout
	// (48 bytes = 12 floats per instance with padding).
	memcpy(dst, src, _active_count * sizeof(TentacleInstance));

	// Upload to GPU storage buffer.
	_rd->buffer_update(_instance_buffer, 0, _active_count * sizeof(TentacleInstance), dst);
}

void TentacleRenderer::clear_tentacles() {
	_active_count = 0;
	if (_instance_buffer.is_valid() && _rd) {
		Vector<uint8_t> zero;
		zero.resize(_buffer_capacity_bytes());
		memset(zero.ptrw(), 0, zero.size());
		_rd->buffer_update(_instance_buffer, 0, zero.size(), zero.ptr());
	}
}

RID TentacleRenderer::render(const Transform3D &p_camera_transform, const Projection &p_camera_projection, float p_time) {
	ERR_FAIL_COND_V(!_initialized, RID());
	ERR_FAIL_NULL_V(_rd, RID());

	if (_active_count == 0) {
		// Still need to clear the framebuffer to transparent.
		Vector<Color> clear_colors;
		clear_colors.push_back(Color(0, 0, 0, 0));
		RD::DrawListID draw_list = _rd->draw_list_begin(
				_framebuffer,
				RD::DRAW_CLEAR_ALL,
				clear_colors,
				1.0f, 0,
				Rect2());
		_rd->draw_list_end();
		_rd->submit();
		_rd->sync();
		return _color_texture;
	}

	// ---- Build push constant ----
	PushConstant pc;
	pc.segments = _segments;
	pc.tentacle_count = uint32_t(_active_count);
	pc.time = p_time;
	pc.noise_amplitude = _noise_amplitude;
	pc.noise_freq = _noise_frequency;
	pc.noise_speed = _noise_speed;
	pc.taper_amount = _taper_amount;
	pc.width_scale = _width_scale;

	// Camera view-projection matrix (column-major).
	Projection vp = p_camera_projection * Projection(p_camera_transform.affine_inverse());
	// Flatten column-major: view_proj[col * 4 + row] = columns[col][row]
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			pc.view_proj[col * 4 + row] = vp.columns[col][row];
		}
	}

	// Camera position.
	Vector3 cam = p_camera_transform.origin;
	pc.cam_pos[0] = cam.x;
	pc.cam_pos[1] = cam.y;
	pc.cam_pos[2] = cam.z;
	pc.cam_pos[3] = 0.0f;

	// ---- Draw ----
	Color clear_color(0, 0, 0, 0);
	Vector<Color> clear_colors;
	clear_colors.push_back(clear_color);

	RD::DrawListID draw_list = _rd->draw_list_begin(
			_framebuffer,
			RD::DRAW_CLEAR_ALL,
			clear_colors,
			1.0f, 0,
			Rect2());

	_rd->draw_list_bind_render_pipeline(draw_list, _pipeline);
	_rd->draw_list_bind_uniform_set(draw_list, _uniform_set, 0);
	_rd->draw_list_set_push_constant(draw_list, &pc, sizeof(PushConstant));

	// Procedural draw: no indices, `tentacle_count` instances, `segments * 2` vertices.
	uint32_t verts_per_instance = _segments * 2;
	_rd->draw_list_draw(draw_list, false, _active_count, verts_per_instance);

	_rd->draw_list_end();
	_rd->submit();
	_rd->sync();

	return _color_texture;
}

PackedByteArray TentacleRenderer::get_color_texture_data() const {
	ERR_FAIL_COND_V(!_initialized, PackedByteArray());
	ERR_FAIL_NULL_V(_rd, PackedByteArray());

	return _rd->texture_get_data(_color_texture, 0);
}

// ============================================================================
// Lifecycle
// ============================================================================

TentacleRenderer::TentacleRenderer() {
}

TentacleRenderer::~TentacleRenderer() {
	_cleanup_rd_resources();
}

// ============================================================================
// Binding
// ============================================================================

void TentacleRenderer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize"), &TentacleRenderer::initialize);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &TentacleRenderer::set_size);
	ClassDB::bind_method(D_METHOD("set_max_tentacles", "count"), &TentacleRenderer::set_max_tentacles);
	ClassDB::bind_method(D_METHOD("set_segments", "segments"), &TentacleRenderer::set_segments);
	ClassDB::bind_method(D_METHOD("set_noise_amplitude", "value"), &TentacleRenderer::set_noise_amplitude);
	ClassDB::bind_method(D_METHOD("set_noise_frequency", "value"), &TentacleRenderer::set_noise_frequency);
	ClassDB::bind_method(D_METHOD("set_noise_speed", "value"), &TentacleRenderer::set_noise_speed);
	ClassDB::bind_method(D_METHOD("set_taper_amount", "value"), &TentacleRenderer::set_taper_amount);
	ClassDB::bind_method(D_METHOD("set_width_scale", "value"), &TentacleRenderer::set_width_scale);
	ClassDB::bind_method(D_METHOD("set_tentacle_data", "data", "count"), &TentacleRenderer::set_tentacle_data);
	ClassDB::bind_method(D_METHOD("clear_tentacles"), &TentacleRenderer::clear_tentacles);
	ClassDB::bind_method(D_METHOD("render", "camera_transform", "camera_projection", "time"), &TentacleRenderer::render);
	ClassDB::bind_method(D_METHOD("get_color_texture"), &TentacleRenderer::get_color_texture);
	ClassDB::bind_method(D_METHOD("get_color_texture_data"), &TentacleRenderer::get_color_texture_data);
	ClassDB::bind_method(D_METHOD("is_initialized"), &TentacleRenderer::is_initialized);
	ClassDB::bind_method(D_METHOD("get_size"), &TentacleRenderer::get_size);
	ClassDB::bind_method(D_METHOD("get_active_count"), &TentacleRenderer::get_active_count);
}
