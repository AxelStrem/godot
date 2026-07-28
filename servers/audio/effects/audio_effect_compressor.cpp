/**************************************************************************/
/*  audio_effect_compressor.cpp                                           */
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

#include "audio_effect_compressor.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "servers/audio/audio_server.h"

AudioEffectCompressorInstance::AudioEffectCompressorInstance() {
	rundb = 0;
	averatio = 0;
	runratio = 0;
	runmax = 0;
	maxover = 0;
	gr_meter = 1.0;
	upward_rundb = 0;
	current_channel = -1;
}

void AudioEffectCompressorInstance::process(const AudioFrame *p_src_frames, AudioFrame *p_dst_frames, int p_frame_count) {
	float threshold = Math::db_to_linear(base->threshold);
	float sample_rate = AudioServer::get_singleton()->get_mix_rate();

	float ratatcoef = std::exp(-1 / (0.00001f * sample_rate));
	float ratrelcoef = std::exp(-1 / (0.5f * sample_rate));
	float attime = base->attack_us / 1000000.0;
	float reltime = base->release_ms / 1000.0;
	float atcoef = std::exp(-1 / (attime * sample_rate));
	float relcoef = std::exp(-1 / (reltime * sample_rate));

	float makeup = Math::db_to_linear(base->gain);

	float mix = base->mix;
	float gr_meter_decay = std::exp(1 / (1 * sample_rate));

	// Upward compression parameters.
	float upward_threshold_db = base->upward_threshold_db;
	float upward_threshold_linear = 0.0f;
	float upward_atcoef = 0.0f;
	float upward_relcoef = 0.0f;
	float upward_ratio = 1.0f;
	bool upward_enabled = false;
	if (upward_threshold_db > -100.0f && base->upward_ratio > 1.0f) {
		upward_enabled = true;
		upward_threshold_linear = Math::db_to_linear(upward_threshold_db);
		float upward_attime = base->upward_attack_us / 1000000.0;
		float upward_reltime = base->upward_release_ms / 1000.0;
		upward_atcoef = std::exp(-1 / (upward_attime * sample_rate));
		upward_relcoef = std::exp(-1 / (upward_reltime * sample_rate));
		upward_ratio = base->upward_ratio;
	}

	const AudioFrame *src = p_src_frames;

	if (base->sidechain != StringName() && current_channel != -1) {
		int bus = AudioServer::get_singleton()->thread_find_bus_index(base->sidechain);
		if (bus >= 0) {
			src = AudioServer::get_singleton()->thread_get_channel_mix_buffer(bus, current_channel);
		}
	}

	for (int i = 0; i < p_frame_count; i++) {
		AudioFrame s = src[i];
		//convert to positive
		s.left = Math::abs(s.left);
		s.right = Math::abs(s.right);

		float peak = MAX(s.left, s.right);

		float overdb = 2.08136898f * Math::linear_to_db(peak / threshold);

		if (overdb < 0.0) { //we only care about what goes over to compress
			overdb = 0.0;
		}

		if (overdb - rundb > 5) { // diffeence is too large
			averatio = 4;
		}

		if (overdb > rundb) {
			rundb = overdb + atcoef * (rundb - overdb);
			runratio = averatio + ratatcoef * (runratio - averatio);
		} else {
			rundb = overdb + relcoef * (rundb - overdb);
			runratio = averatio + ratrelcoef * (runratio - averatio);
		}

		// Flush denormals to avoid performance issues on some CPUs.
		if (Math::abs(rundb) < 1e-30f) { rundb = 0.0f; }
		if (Math::abs(runratio) < 1e-30f) { runratio = 0.0f; }

		overdb = rundb;
		averatio = runratio;

		float cratio;

		if (false) { //rato all-in
			cratio = 12 + averatio;
		} else {
			cratio = base->ratio;
		}

		float gr = -overdb * (cratio - 1) / cratio;
		float grv = Math::db_to_linear(gr);

		runmax = maxover + relcoef * (runmax - maxover); // highest peak for setting att/rel decays in reltime
		maxover = runmax;

		// Flush denormals.
		if (Math::abs(runmax) < 1e-30f) { runmax = 0.0f; }
		if (Math::abs(maxover) < 1e-30f) { maxover = 0.0f; }

		if (grv < gr_meter) {
			gr_meter = grv;
		} else {
			gr_meter *= gr_meter_decay;
			if (gr_meter > 1) {
				gr_meter = 1;
			}
		}

		// Upward compression: boost signals below the upward threshold.
		float upward_boost = 1.0f;
		if (upward_enabled && peak > 1e-30f) {
			float underdb = 2.08136898f * Math::linear_to_db(upward_threshold_linear / peak);
			if (underdb < 0.0) {
				underdb = 0.0;
			}

			// Clamp underdb to prevent Math::db_to_linear overflow.
			// At underdb=60, worst-case boost is ~43 dB (100x), which is
			// well within float range and more than enough for any musical
			// upward compression. Anything below ~69 dBFS gets the same boost.
			if (underdb > 60.0f) {
				underdb = 60.0f;
			}

			if (underdb > upward_rundb) {
				upward_rundb = underdb + upward_atcoef * (upward_rundb - underdb);
			} else {
				upward_rundb = underdb + upward_relcoef * (upward_rundb - underdb);
			}

			// Flush denormals.
			if (Math::abs(upward_rundb) < 1e-30f) { upward_rundb = 0.0f; }

			float upward_boost_db = upward_rundb * (upward_ratio - 1.0f) / upward_ratio;
			upward_boost = Math::db_to_linear(upward_boost_db);
		}

		p_dst_frames[i] = p_src_frames[i] * grv * upward_boost * makeup * mix + p_src_frames[i] * (1.0 - mix);
	}
}

Ref<AudioEffectInstance> AudioEffectCompressor::instantiate() {
	Ref<AudioEffectCompressorInstance> ins;
	ins.instantiate();
	ins->base = Ref<AudioEffectCompressor>(this);
	ins->rundb = 0;
	ins->runratio = 0;
	ins->averatio = 0;
	ins->runmax = 0;
	ins->maxover = 0;
	ins->gr_meter = 1.0;
	ins->upward_rundb = 0;
	ins->current_channel = -1;
	return ins;
}

void AudioEffectCompressor::set_threshold(float p_threshold) {
	threshold = p_threshold;
}

float AudioEffectCompressor::get_threshold() const {
	return threshold;
}

void AudioEffectCompressor::set_ratio(float p_ratio) {
	ratio = p_ratio;
}

float AudioEffectCompressor::get_ratio() const {
	return ratio;
}

void AudioEffectCompressor::set_gain(float p_gain) {
	gain = p_gain;
}

float AudioEffectCompressor::get_gain() const {
	return gain;
}

void AudioEffectCompressor::set_attack_us(float p_attack_us) {
	attack_us = p_attack_us;
}

float AudioEffectCompressor::get_attack_us() const {
	return attack_us;
}

void AudioEffectCompressor::set_release_ms(float p_release_ms) {
	release_ms = p_release_ms;
}

float AudioEffectCompressor::get_release_ms() const {
	return release_ms;
}

void AudioEffectCompressor::set_mix(float p_mix) {
	mix = p_mix;
}

float AudioEffectCompressor::get_mix() const {
	return mix;
}

void AudioEffectCompressor::set_sidechain(const StringName &p_sidechain) {
	AudioServer::get_singleton()->lock();
	sidechain = p_sidechain;
	AudioServer::get_singleton()->unlock();
}

StringName AudioEffectCompressor::get_sidechain() const {
	return sidechain;
}

void AudioEffectCompressor::set_upward_threshold_db(float p_threshold) {
	upward_threshold_db = p_threshold;
}

float AudioEffectCompressor::get_upward_threshold_db() const {
	return upward_threshold_db;
}

void AudioEffectCompressor::set_upward_ratio(float p_ratio) {
	upward_ratio = p_ratio;
}

float AudioEffectCompressor::get_upward_ratio() const {
	return upward_ratio;
}

void AudioEffectCompressor::set_upward_attack_us(float p_attack_us) {
	upward_attack_us = p_attack_us;
}

float AudioEffectCompressor::get_upward_attack_us() const {
	return upward_attack_us;
}

void AudioEffectCompressor::set_upward_release_ms(float p_release_ms) {
	upward_release_ms = p_release_ms;
}

float AudioEffectCompressor::get_upward_release_ms() const {
	return upward_release_ms;
}

void AudioEffectCompressor::_validate_property(PropertyInfo &p_property) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (p_property.name == "sidechain") {
		String buses = "";
		for (int i = 0; i < AudioServer::get_singleton()->get_bus_count(); i++) {
			buses += ",";
			buses += AudioServer::get_singleton()->get_bus_name(i);
		}

		p_property.hint_string = buses;
	}
}

void AudioEffectCompressor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_threshold", "threshold"), &AudioEffectCompressor::set_threshold);
	ClassDB::bind_method(D_METHOD("get_threshold"), &AudioEffectCompressor::get_threshold);

	ClassDB::bind_method(D_METHOD("set_ratio", "ratio"), &AudioEffectCompressor::set_ratio);
	ClassDB::bind_method(D_METHOD("get_ratio"), &AudioEffectCompressor::get_ratio);

	ClassDB::bind_method(D_METHOD("set_gain", "gain"), &AudioEffectCompressor::set_gain);
	ClassDB::bind_method(D_METHOD("get_gain"), &AudioEffectCompressor::get_gain);

	ClassDB::bind_method(D_METHOD("set_attack_us", "attack_us"), &AudioEffectCompressor::set_attack_us);
	ClassDB::bind_method(D_METHOD("get_attack_us"), &AudioEffectCompressor::get_attack_us);

	ClassDB::bind_method(D_METHOD("set_release_ms", "release_ms"), &AudioEffectCompressor::set_release_ms);
	ClassDB::bind_method(D_METHOD("get_release_ms"), &AudioEffectCompressor::get_release_ms);

	ClassDB::bind_method(D_METHOD("set_mix", "mix"), &AudioEffectCompressor::set_mix);
	ClassDB::bind_method(D_METHOD("get_mix"), &AudioEffectCompressor::get_mix);

	ClassDB::bind_method(D_METHOD("set_sidechain", "sidechain"), &AudioEffectCompressor::set_sidechain);
	ClassDB::bind_method(D_METHOD("get_sidechain"), &AudioEffectCompressor::get_sidechain);

	ClassDB::bind_method(D_METHOD("set_upward_threshold_db", "threshold"), &AudioEffectCompressor::set_upward_threshold_db);
	ClassDB::bind_method(D_METHOD("get_upward_threshold_db"), &AudioEffectCompressor::get_upward_threshold_db);

	ClassDB::bind_method(D_METHOD("set_upward_ratio", "ratio"), &AudioEffectCompressor::set_upward_ratio);
	ClassDB::bind_method(D_METHOD("get_upward_ratio"), &AudioEffectCompressor::get_upward_ratio);

	ClassDB::bind_method(D_METHOD("set_upward_attack_us", "attack_us"), &AudioEffectCompressor::set_upward_attack_us);
	ClassDB::bind_method(D_METHOD("get_upward_attack_us"), &AudioEffectCompressor::get_upward_attack_us);

	ClassDB::bind_method(D_METHOD("set_upward_release_ms", "release_ms"), &AudioEffectCompressor::set_upward_release_ms);
	ClassDB::bind_method(D_METHOD("get_upward_release_ms"), &AudioEffectCompressor::get_upward_release_ms);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "threshold", PROPERTY_HINT_RANGE, "-60,0,0.1,suffix:dB"), "set_threshold", "get_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ratio", PROPERTY_HINT_RANGE, "1,48,0.1"), "set_ratio", "get_ratio");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gain", PROPERTY_HINT_RANGE, "-20,20,0.1,suffix:dB"), "set_gain", "get_gain");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "attack_us", PROPERTY_HINT_RANGE, U"20,2000,1,suffix:\u00B5s"), "set_attack_us", "get_attack_us");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "release_ms", PROPERTY_HINT_RANGE, "20,2000,1,suffix:ms"), "set_release_ms", "get_release_ms");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mix", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_mix", "get_mix");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "sidechain", PROPERTY_HINT_ENUM), "set_sidechain", "get_sidechain");

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "upward_threshold_db", PROPERTY_HINT_RANGE, "-200,0,0.1"), "set_upward_threshold_db", "get_upward_threshold_db");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "upward_ratio", PROPERTY_HINT_RANGE, "1,48,0.1"), "set_upward_ratio", "get_upward_ratio");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "upward_attack_us", PROPERTY_HINT_RANGE, U"20,20000,1,suffix:\u00B5s"), "set_upward_attack_us", "get_upward_attack_us");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "upward_release_ms", PROPERTY_HINT_RANGE, "20,2000,1,suffix:ms"), "set_upward_release_ms", "get_upward_release_ms");
}

AudioEffectCompressor::AudioEffectCompressor() {
	threshold = 0;
	ratio = 4;
	gain = 0;
	attack_us = 20;
	release_ms = 250;
	mix = 1;
	upward_threshold_db = -200.0f;
	upward_ratio = 1.0f;
	upward_attack_us = 5000;
	upward_release_ms = 300;
}
