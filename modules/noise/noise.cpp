/**************************************************************************/
/*  noise.cpp                                                             */
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

#include "noise.h"

#include <cstring>

#include "core/math/math_funcs.h"
#include "core/string/print_string.h"

namespace {

template <typename T>
static void blur_slice(const T *p_src, T *p_dst, int p_width, int p_height, const Vector<float> &p_kernel, bool p_wrap_xy, float p_max_value) {
	ERR_FAIL_NULL(p_src);
	ERR_FAIL_NULL(p_dst);
	const int radius = p_kernel.size() / 2;
	Vector<float> temp;
	temp.resize(p_width * p_height);
	float *temp_ptr = temp.ptrw();
	auto wrap_index = [](int p_value, int p_limit, bool p_wrap) {
		if (p_wrap && p_limit > 0) {
			int value = p_value % p_limit;
			if (value < 0) {
				value += p_limit;
			}
			return value;
		}
		return CLAMP(p_value, 0, p_limit - 1);
	};

	for (int y = 0; y < p_height; y++) {
		for (int x = 0; x < p_width; x++) {
			float value = 0.0f;
			for (int k = -radius; k <= radius; k++) {
				const int sample_x = wrap_index(x + k, p_width, p_wrap_xy);
				value += p_kernel[radius + k] * static_cast<float>(p_src[sample_x + y * p_width]);
			}
			temp_ptr[x + y * p_width] = value;
		}
	}

	const float *temp_read = temp.ptr();
	for (int y = 0; y < p_height; y++) {
		for (int x = 0; x < p_width; x++) {
			float value = 0.0f;
			for (int k = -radius; k <= radius; k++) {
				const int sample_y = wrap_index(y + k, p_height, p_wrap_xy);
				value += p_kernel[radius + k] * temp_read[x + sample_y * p_width];
			}
			value = CLAMP(value, 0.0f, p_max_value);
			p_dst[x + y * p_width] = static_cast<T>(Math::round(value));
		}
	}
}

static void build_gaussian_kernel(Vector<float> &r_kernel, float p_strength) {
	const float sigma = MAX(p_strength, 0.01f);
	const int radius = CLAMP(Math::ceil(sigma * 3.0f), 1, 64);
	const int kernel_size = radius * 2 + 1;
	r_kernel.resize(kernel_size);
	float weight_sum = 0.0f;
	for (int i = -radius; i <= radius; i++) {
		const float weight = Math::exp(-(i * i) / (2.0f * sigma * sigma));
		r_kernel.write[i + radius] = weight;
		weight_sum += weight;
	}
	const float inv_sum = 1.0f / weight_sum;
	for (int i = 0; i < kernel_size; i++) {
		r_kernel.write[i] *= inv_sum;
	}
}

template <typename T>
static void blur_images_impl(Vector<Ref<Image>> &r_images, float p_strength, bool p_wrap_xy, bool p_wrap_z) {
	if (r_images.is_empty()) {
		return;
	}

	const int width = r_images[0]->get_width();
	const int height = r_images[0]->get_height();
	const int depth = r_images.size();
	const float max_value = sizeof(T) == 1 ? 255.0f : 65535.0f;

	Vector<float> kernel;
	build_gaussian_kernel(kernel, p_strength);
	const int radius = kernel.size() / 2;

	Vector<Vector<T>> slices;
	slices.resize(depth);

	auto wrap_index = [](int p_value, int p_limit, bool p_wrap) {
		if (p_wrap && p_limit > 0) {
			int value = p_value % p_limit;
			if (value < 0) {
				value += p_limit;
			}
			return value;
		}
		return CLAMP(p_value, 0, p_limit - 1);
	};

	for (int z = 0; z < depth; z++) {
		const Ref<Image> img = r_images[z];
		ERR_CONTINUE(img.is_null());
		ERR_CONTINUE(img->get_width() != width || img->get_height() != height);

		const Vector<uint8_t> raw = img->get_data();
		ERR_CONTINUE(raw.size() != width * height * sizeof(T));
		const T *src = reinterpret_cast<const T *>(raw.ptr());

		Vector<T> dst;
		dst.resize(width * height);
		blur_slice(src, dst.ptrw(), width, height, kernel, p_wrap_xy, max_value);
		slices.write[z] = dst;
	}

	if (depth > 1) {
		Vector<Vector<T>> depth_src = slices;
		Vector<Vector<T>> depth_dst;
		depth_dst.resize(depth);

		for (int z = 0; z < depth; z++) {
			Vector<T> dest;
			dest.resize(width * height);
			T *dest_ptr = dest.ptrw();

			for (int idx = 0; idx < width * height; idx++) {
				float value = 0.0f;
				for (int k = -radius; k <= radius; k++) {
					const int sample_z = wrap_index(z + k, depth, p_wrap_z);
					value += kernel[radius + k] * static_cast<float>(depth_src[sample_z][idx]);
				}
				value = CLAMP(value, 0.0f, max_value);
				dest_ptr[idx] = static_cast<T>(Math::round(value));
			}
			depth_dst.write[z] = dest;
		}
		slices = depth_dst;
	}

	for (int z = 0; z < depth; z++) {
		const Vector<T> &slice = slices[z];
		Vector<uint8_t> dest_bytes;
		dest_bytes.resize(slice.size() * sizeof(T));
		std::memcpy(dest_bytes.ptrw(), slice.ptr(), dest_bytes.size());
		Ref<Image> new_image = memnew(Image(width, height, false, r_images[z]->get_format(), dest_bytes));
		r_images.write[z] = new_image;
	}
}

} // namespace

void Noise::apply_blur(Vector<Ref<Image>> &r_images, float p_strength, bool p_wrap_xy, bool p_wrap_z) {
	if (r_images.is_empty()) {
		return;
	}
	if (Math::is_zero_approx(p_strength)) {
		return;
	}

	Image::Format format = r_images[0]->get_format();
	for (int i = 0; i < r_images.size(); i++) {
		if (r_images[i].is_null()) {
			return;
		}
		if (r_images[i]->get_format() != format) {
			ERR_FAIL_MSG("Noise::apply_blur requires all images to share the same format.");
		}
	}

	switch (format) {
		case Image::FORMAT_L8:
		case Image::FORMAT_R8:
			blur_images_impl<uint8_t>(r_images, p_strength, p_wrap_xy, p_wrap_z);
			break;
		case Image::FORMAT_L16:
		case Image::FORMAT_R16:
			blur_images_impl<uint16_t>(r_images, p_strength, p_wrap_xy, p_wrap_z);
			break;
		default:
			WARN_PRINT_ONCE(vformat("Noise smoothing currently supports single-channel 8-bit or 16-bit formats. Skipping blur for %s.", Image::format_names[format]));
			break;
	}
}


Vector<Ref<Image>> Noise::_get_seamless_image(int p_width, int p_height, int p_depth, bool p_invert, bool p_in_3d_space, real_t p_blend_skirt, bool p_normalize, Image::Format p_format) const {
	ERR_FAIL_COND_V(p_width <= 0 || p_height <= 0 || p_depth <= 0, Vector<Ref<Image>>());

	int skirt_width = MAX(1, p_width * p_blend_skirt);
	int skirt_height = MAX(1, p_height * p_blend_skirt);
	int skirt_depth = MAX(1, p_depth * p_blend_skirt);
	int src_width = p_width + skirt_width;
	int src_height = p_height + skirt_height;
	int src_depth = p_depth + skirt_depth;

	Vector<Ref<Image>> src = _get_image(src_width, src_height, src_depth, p_invert, p_in_3d_space, p_normalize, p_format);
	Image::Format src_format = src[0]->get_format();

	switch (src_format) {
		case Image::FORMAT_L8:
			return _generate_seamless_image<uint8_t>(src, p_width, p_height, p_depth, p_invert, p_blend_skirt);
		case Image::FORMAT_L16:
		case Image::FORMAT_LH:
			return _generate_seamless_image<uint16_t>(src, p_width, p_height, p_depth, p_invert, p_blend_skirt);
		case Image::FORMAT_LF:
			return _generate_seamless_image<float>(src, p_width, p_height, p_depth, p_invert, p_blend_skirt);
		default:
			return _generate_seamless_image<uint32_t>(src, p_width, p_height, p_depth, p_invert, p_blend_skirt);
	}
}

Ref<Image> Noise::get_seamless_image(int p_width, int p_height, bool p_invert, bool p_in_3d_space, real_t p_blend_skirt, bool p_normalize, Image::Format p_format) const {
	Vector<Ref<Image>> images = _get_seamless_image(p_width, p_height, 1, p_invert, p_in_3d_space, p_blend_skirt, p_normalize, p_format);
	if (images.is_empty()) {
		return Ref<Image>();
	}
	return images[0];
}

TypedArray<Image> Noise::get_seamless_image_3d(int p_width, int p_height, int p_depth, bool p_invert, real_t p_blend_skirt, bool p_normalize, Image::Format p_format) const {
	Vector<Ref<Image>> images = _get_seamless_image(p_width, p_height, p_depth, p_invert, true, p_blend_skirt, p_normalize, p_format);

	TypedArray<Image> ret;
	ret.resize(images.size());
	for (int i = 0; i < images.size(); i++) {
		ret[i] = images[i];
	}
	return ret;
}

// Template specialization for faster grayscale blending.
template <>
uint8_t Noise::_alpha_blend<uint8_t>(uint8_t p_bg, uint8_t p_fg, int p_alpha) const {
	uint16_t alpha = p_alpha + 1;
	uint16_t inv_alpha = 256 - p_alpha;

	return (uint8_t)((alpha * p_fg + inv_alpha * p_bg) >> 8);
}

Vector<Ref<Image>> Noise::_get_image(int p_width, int p_height, int p_depth, bool p_invert, bool p_in_3d_space, bool p_normalize, Image::Format p_format) const {
	ERR_FAIL_COND_V(p_width <= 0 || p_height <= 0 || p_depth <= 0, Vector<Ref<Image>>());

	auto is_supported_format = [](Image::Format p_format) {
		return p_format == Image::FORMAT_L8 || p_format == Image::FORMAT_L16 || p_format == Image::FORMAT_LH || p_format == Image::FORMAT_LF;
	};

	Image::Format format = p_format;
	if (!is_supported_format(format)) {
		ERR_PRINT("Unsupported noise image format requested, defaulting to FORMAT_L8.");
		format = Image::FORMAT_L8;
	}

	Vector<Ref<Image>> images;
	images.resize(p_depth);

	const int pixel_count_per_slice = p_width * p_height;
	const int pixel_size = Image::get_format_pixel_size(format);

	auto pack_normalized_value = [&](uint8_t *p_buffer, int p_index, real_t p_value) {
		real_t normalized = CLAMP(p_value, 0.0, 1.0);
		if (p_invert) {
			normalized = 1.0 - normalized;
		}
		switch (format) {
			case Image::FORMAT_L8: {
				reinterpret_cast<uint8_t *>(p_buffer)[p_index] = uint8_t(CLAMP(Math::round(normalized * 255.0), 0.0, 255.0));
			} break;
			case Image::FORMAT_L16: {
				reinterpret_cast<uint16_t *>(p_buffer)[p_index] = uint16_t(CLAMP(Math::round(normalized * 65535.0), 0.0, 65535.0));
			} break;
			case Image::FORMAT_LH: {
				reinterpret_cast<uint16_t *>(p_buffer)[p_index] = Math::make_half_float((float)normalized);
			} break;
			case Image::FORMAT_LF: {
				reinterpret_cast<float *>(p_buffer)[p_index] = (float)normalized;
			} break;
			default: {
				reinterpret_cast<uint8_t *>(p_buffer)[p_index] = uint8_t(CLAMP(Math::round(normalized * 255.0), 0.0, 255.0));
			} break;
		}
	};

	if (p_normalize) {
		// Get all values and identify min/max values.
		LocalVector<real_t> values;
		values.resize(p_width * p_height * p_depth);

		real_t min_val = FLT_MAX;
		real_t max_val = -FLT_MAX;
		int idx = 0;
		for (int d = 0; d < p_depth; d++) {
			for (int y = 0; y < p_height; y++) {
				for (int x = 0; x < p_width; x++) {
					values[idx] = p_in_3d_space ? get_noise_3d(x, y, d) : get_noise_2d(x, y);
					if (values[idx] > max_val) {
						max_val = values[idx];
					}
					if (values[idx] < min_val) {
						min_val = values[idx];
					}
					idx++;
				}
			}
		}
		idx = 0;
		// Normalize values and write to texture.
		for (int d = 0; d < p_depth; d++) {
			Vector<uint8_t> data;
			data.resize(pixel_count_per_slice * pixel_size);

			uint8_t *write_ptr = data.ptrw();

			for (int y = 0; y < p_height; y++) {
				for (int x = 0; x < p_width; x++) {
					real_t normalized = 0.0;
					if (max_val != min_val) {
						normalized = (values[idx] - min_val) / (max_val - min_val);
					}
					pack_normalized_value(write_ptr, x + y * p_width, normalized);
					idx++;
				}
			}
			Ref<Image> img = memnew(Image(p_width, p_height, false, format, data));
			images.write[d] = img;
		}
	} else {
		// Without normalization, the expected range of the noise function is [-1, 1].

		for (int d = 0; d < p_depth; d++) {
			Vector<uint8_t> data;
			data.resize(pixel_count_per_slice * pixel_size);

			uint8_t *write_ptr = data.ptrw();
			int idx = 0;
			for (int y = 0; y < p_height; y++) {
				for (int x = 0; x < p_width; x++) {
					float value = (p_in_3d_space ? get_noise_3d(x, y, d) : get_noise_2d(x, y));
					real_t normalized = CLAMP(real_t(value) * 0.5 + 0.5, 0.0, 1.0);
					pack_normalized_value(write_ptr, idx, normalized);
					idx++;
				}
			}

			Ref<Image> img = memnew(Image(p_width, p_height, false, format, data));
			images.write[d] = img;
		}
	}

	return images;
}

Ref<Image> Noise::get_image(int p_width, int p_height, bool p_invert, bool p_in_3d_space, bool p_normalize, Image::Format p_format) const {
	Vector<Ref<Image>> images = _get_image(p_width, p_height, 1, p_invert, p_in_3d_space, p_normalize, p_format);
	if (images.is_empty()) {
		return Ref<Image>();
	}
	return images[0];
}

TypedArray<Image> Noise::get_image_3d(int p_width, int p_height, int p_depth, bool p_invert, bool p_normalize, Image::Format p_format) const {
	Vector<Ref<Image>> images = _get_image(p_width, p_height, p_depth, p_invert, true, p_normalize, p_format);

	TypedArray<Image> ret;
	ret.resize(images.size());
	for (int i = 0; i < images.size(); i++) {
		ret[i] = images[i];
	}
	return ret;
}

void Noise::_bind_methods() {
	// Noise functions.
	ClassDB::bind_method(D_METHOD("get_noise_1d", "x"), &Noise::get_noise_1d);
	ClassDB::bind_method(D_METHOD("get_noise_2d", "x", "y"), &Noise::get_noise_2d);
	ClassDB::bind_method(D_METHOD("get_noise_2dv", "v"), &Noise::get_noise_2dv);
	ClassDB::bind_method(D_METHOD("get_noise_3d", "x", "y", "z"), &Noise::get_noise_3d);
	ClassDB::bind_method(D_METHOD("get_noise_3dv", "v"), &Noise::get_noise_3dv);

	// Textures.
	ClassDB::bind_method(D_METHOD("get_image", "width", "height", "invert", "in_3d_space", "normalize", "format"), &Noise::get_image, DEFVAL(false), DEFVAL(false), DEFVAL(true), DEFVAL(Image::FORMAT_L8));
	ClassDB::bind_method(D_METHOD("get_seamless_image", "width", "height", "invert", "in_3d_space", "skirt", "normalize", "format"), &Noise::get_seamless_image, DEFVAL(false), DEFVAL(false), DEFVAL(0.1), DEFVAL(true), DEFVAL(Image::FORMAT_L8));
	ClassDB::bind_method(D_METHOD("get_image_3d", "width", "height", "depth", "invert", "normalize", "format"), &Noise::get_image_3d, DEFVAL(false), DEFVAL(true), DEFVAL(Image::FORMAT_L8));
	ClassDB::bind_method(D_METHOD("get_seamless_image_3d", "width", "height", "depth", "invert", "skirt", "normalize", "format"), &Noise::get_seamless_image_3d, DEFVAL(false), DEFVAL(0.1), DEFVAL(true), DEFVAL(Image::FORMAT_L8));
}
