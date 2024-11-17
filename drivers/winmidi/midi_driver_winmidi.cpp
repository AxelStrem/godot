/**************************************************************************/
/*  midi_driver_winmidi.cpp                                               */
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

#ifdef WINMIDI_ENABLED

#include "midi_driver_winmidi.h"

#include "core/string/print_string.h"

void MIDIDriverWinMidi::read(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
	if (wMsg == MIM_DATA) {
		int midi_input = dynamic_cast<MIDIDriverWinMidi*>(get_singleton())->connected_sources.find(hMidiIn);
		receive_input_packet(midi_input, (uint64_t)dwParam2, (uint8_t *)&dwParam1, 3);
	}
}

Error MIDIDriverWinMidi::open() {
	auto num_devices = midiInGetNumDevs();
	char nd[2];
	nd[0]='0'+num_devices;
	nd[1]=0;
	UINT sz = 0;
	for (UINT i = 0; i < num_devices; i++) {
		HMIDIIN midi_in;

		String src_name = "";
		{
			MIDIINCAPS caps;
			MMRESULT res = midiInGetDevCaps(i, &caps, sizeof(MIDIINCAPS));
			if (res == MMSYSERR_NOERROR) {
				src_name = String(caps.szPname);
			}
		}

		MMRESULT res = midiInOpen(&midi_in, i, (DWORD_PTR)read, (DWORD_PTR)this, CALLBACK_FUNCTION);
		if (res == MMSYSERR_NOERROR) {
			midiInStart(midi_in);
			connected_sources.insert(sz, midi_in);
			source_names.push_back(src_name);
			sz++;
		} else {
			midiInReset(midi_in);
		}
	}
	return OK;
}

PackedStringArray MIDIDriverWinMidi::get_connected_inputs() const {
	PackedStringArray list;

	for (int i = 0; i < connected_sources.size(); i++) {
		list.push_back(source_names[i]);
	}

	return list;
}

void MIDIDriverWinMidi::close() {
	for (int i = 0; i < connected_sources.size(); i++) {
		HMIDIIN midi_in = connected_sources[i];
		midiInStop(midi_in);
		midiInClose(midi_in);
	}
	connected_sources.clear();
	source_names.clear();
}

MIDIDriverWinMidi::MIDIDriverWinMidi() {
}

MIDIDriverWinMidi::~MIDIDriverWinMidi() {
	close();
}

#endif // WINMIDI_ENABLED
