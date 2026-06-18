/**************************************************************************/
/*  audio_effect_send.cpp                                                 */
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

#include "audio_effect_send.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "servers/audio/audio_server.h"

void AudioEffectSendInstance::process(const AudioFrame *p_src_frames, AudioFrame *p_dst_frames, int p_frame_count) {
	// Determine the target bus index.
	AudioServer *server = AudioServer::get_singleton();
	int target_bus_idx = server->thread_find_bus_index(base->send_bus);

	// Feedback protection: if the target bus has an equal or higher index than the
	// source bus, redirect to the Master bus (index 0) to prevent feedback loops.
	// This matches the existing bus-send behavior in AudioServer::_mix_step().
	if (target_bus_idx >= source_bus_index) {
		target_bus_idx = 0;
	}

	// Get the target bus channel buffer.
	AudioFrame *target_buf = server->thread_get_channel_mix_buffer(target_bus_idx, channel_index);

	// Interpolate send amount to avoid clicks when the parameter changes.
	float send_amount_db = base->send_amount_db;
	float send_vol = Math::db_to_linear(mix_send_amount_db);
	float send_vol_inc = (Math::db_to_linear(send_amount_db) - send_vol) / float(p_frame_count);

	for (int i = 0; i < p_frame_count; i++) {
		// Dry signal passes through unchanged.
		p_dst_frames[i] = p_src_frames[i];

		// Send a scaled copy to the target bus.
		target_buf[i] += p_src_frames[i] * send_vol;
		send_vol += send_vol_inc;
	}

	// Store the final send amount for the next mix step.
	mix_send_amount_db = send_amount_db;
}

bool AudioEffectSendInstance::process_silence() const {
	// Always process, since we need to pass through the dry signal even when
	// the send amount is at minimum. The send itself may be silent, but the
	// dry passthrough must continue.
	return false;
}

Ref<AudioEffectInstance> AudioEffectSend::instantiate() {
	Ref<AudioEffectSendInstance> ins;
	ins.instantiate();
	ins->base = Ref<AudioEffectSend>(this);
	ins->send_bus = send_bus;
	ins->mix_send_amount_db = send_amount_db;
	return ins;
}

void AudioEffectSend::set_send_bus(const StringName &p_bus) {
	send_bus = p_bus;
}

StringName AudioEffectSend::get_send_bus() const {
	return send_bus;
}

void AudioEffectSend::set_send_amount_db(float p_amount) {
	send_amount_db = p_amount;
}

float AudioEffectSend::get_send_amount_db() const {
	return send_amount_db;
}

void AudioEffectSend::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_send_bus", "bus"), &AudioEffectSend::set_send_bus);
	ClassDB::bind_method(D_METHOD("get_send_bus"), &AudioEffectSend::get_send_bus);
	ClassDB::bind_method(D_METHOD("set_send_amount_db", "amount"), &AudioEffectSend::set_send_amount_db);
	ClassDB::bind_method(D_METHOD("get_send_amount_db"), &AudioEffectSend::get_send_amount_db);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "send_bus"), "set_send_bus", "get_send_bus");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "send_amount_db", PROPERTY_HINT_RANGE, "-80,24,0.01,suffix:dB"), "set_send_amount_db", "get_send_amount_db");
}

AudioEffectSend::AudioEffectSend() {
}
