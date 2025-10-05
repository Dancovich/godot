/**************************************************************************/
/*  tts_windows.cpp                                                       */
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

#include "tts_windows.h"

#include <winrt/windows.foundation.collections.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.media.core.h>

using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Media::Core;

TTS_Windows *TTS_Windows::singleton = nullptr;

bool TTS_Windows::is_speaking() const {
	ERR_FAIL_NULL_V(media_player, false);
	return media_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing || pending_utterance_id != NO_UTTERANCE_ID;
}

bool TTS_Windows::is_paused() const {
	ERR_FAIL_NULL_V(media_player, false);
	return paused;
}

Array TTS_Windows::get_voices() const {
	Array voices;

	IVectorView<VoiceInformation> winrt_voices = synthesizer.AllVoices();
	for (const VoiceInformation voice : winrt_voices) {
		Dictionary new_voice = {};

		wchar_t const *voice_id = winrt::to_hstring(voice.Id()).c_str();
		wchar_t const *voice_name = winrt::to_hstring(voice.DisplayName()).c_str();
		wchar_t const *voice_language = winrt::to_hstring(voice.Language()).c_str();

		new_voice["id"] = String::utf16((const char16_t *)voice_id);
		new_voice["name"] = String::utf16((const char16_t *)voice_name);
		new_voice["language"] = String::utf16((const char16_t *)voice_language);

		voices.append(new_voice);
	}

	return voices;
}

void TTS_Windows::speak(const String &p_text, const String &p_voice, int p_volume, float p_pitch, float p_rate, int p_utterance_id, bool p_interrupt) {
	ERR_FAIL_NULL(synthesizer);
	ERR_FAIL_NULL(media_player);

	if (p_interrupt) {
		stop();
	}

	if (p_text.is_empty()) {
		DisplayServer::get_singleton()->tts_post_utterance_event(DisplayServer::TTS_UTTERANCE_CANCELED, p_utterance_id);
		return;
	}

	DisplayServer::TTSUtterance message;
	message.text = p_text;
	message.voice = p_voice;
	message.volume = CLAMP(p_volume, 0, 100);
	message.pitch = CLAMP(p_pitch, 0.f, 2.f);
	message.rate = CLAMP(p_rate, 0.1f, 10.f);
	message.id = p_utterance_id;
	queue.push_back(message);

	if (is_paused()) {
		resume();
	}
}

void TTS_Windows::pause() {
	ERR_FAIL_NULL(media_player);

	if (!paused) {
		MediaPlaybackSession session = media_player.PlaybackSession();
		if (session.CanPause()) {
			media_player.Pause();
			paused = true;
		}
	}
}

void TTS_Windows::resume() {
	ERR_FAIL_NULL(media_player);

	if (paused && media_player.Source() != nullptr) {
		media_player.Play();
		paused = false;
	}
}

void TTS_Windows::stop() {
	ERR_FAIL_NULL(media_player);

	MediaPlaybackSession session = media_player.PlaybackSession();
	if (session.CanPause()) {
		media_player.Pause();
	}

	for (DisplayServer::TTSUtterance &message : queue) {
		DisplayServer::get_singleton()->tts_post_utterance_event(DisplayServer::TTS_UTTERANCE_CANCELED, message.id);
	}

	queue.clear();

	int utterance_id = pending_utterance_id;
	if (utterance_id != NO_UTTERANCE_ID) {
		DisplayServer::get_singleton()->tts_post_utterance_event(DisplayServer::TTS_UTTERANCE_CANCELED, utterance_id);
		pending_utterance_id = NO_UTTERANCE_ID;
	}

	media_player.Source(nullptr);
	paused = false;
}

void TTS_Windows::process_events() {
	if (!paused && queue.size() > 0 && !is_speaking()) {
		DisplayServer::TTSUtterance &message = queue.front()->get();

		IVectorView<VoiceInformation> available_voices = synthesizer.AllVoices();
		bool found_voice = false;
		for (impl::fast_iterator<IVectorView<VoiceInformation>> iterator = available_voices.begin(); iterator != available_voices.end(); ++iterator) {
			VoiceInformation current_voice = *iterator;

			wchar_t const *voice_id_utf16 = winrt::to_hstring(current_voice.Id()).c_str();
			if (message.voice == String::utf16((const char16_t *)voice_id_utf16)) {
				synthesizer.Voice(current_voice);
				found_voice = true;
				break;
			}
		}

		if (!found_voice) {
			VoiceInformation default_voice = synthesizer.DefaultVoice();
			synthesizer.Voice(default_voice);
		}

		synthesizer.Options().AudioVolume(message.volume / 100.0);
		synthesizer.Options().AudioPitch(message.pitch);

		// map rate range to WinRT range of 0.5 to 6.0
		double playback_rate = 0.5f + ((message.rate - 0.1f) / (10.0f - 0.1f)) * (6.0f - 0.5f);
		synthesizer.Options().SpeakingRate(playback_rate);

		const char16_t *utf16_data = message.text.utf16().get_data();
		size_t utf16_length = message.text.length();
		hstring converted_text(reinterpret_cast<const wchar_t *>(utf16_data), utf16_length);

		pending_utterance_id = message.id;
		IAsyncOperation<SpeechSynthesisStream> synthesize_task = is_ssml(message)
				? synthesizer.SynthesizeSsmlToStreamAsync(converted_text)
				: synthesizer.SynthesizeTextToStreamAsync(converted_text);
		synthesize_task.Completed([this](IAsyncOperation<SpeechSynthesisStream> const &res, AsyncStatus const status) {
			int utterance_id = pending_utterance_id;
			if (utterance_id == NO_UTTERANCE_ID) {
				return;
			}

			if (media_player == nullptr) {
				print_error("Could not synthesize text to speech - media_player != null is false");
				DisplayServer::get_singleton()->tts_post_utterance_event(DisplayServer::TTS_UTTERANCE_CANCELED, utterance_id);
				pending_utterance_id = NO_UTTERANCE_ID;
			} else if (status != AsyncStatus::Completed) {
				if (status == AsyncStatus::Error) {
					print_error(vformat("Could not synthesize text to speech - error processing text (marlfomed SSML?)"));
				} else if (status == AsyncStatus::Canceled) {
					print_error("Could not synthesize text to speech - operation was canceled");
				} else {
					print_error("Could not synthesize text to speech - unknown error");
				}

				DisplayServer::get_singleton()->tts_post_utterance_event(DisplayServer::TTS_UTTERANCE_CANCELED, utterance_id);
				pending_utterance_id = NO_UTTERANCE_ID;
			} else {
				SpeechSynthesisStream generated_stream = res.GetResults();

				MediaSource source = MediaSource::CreateFromStream(generated_stream, L"Audio");
				MediaPlaybackItem media_item = MediaPlaybackItem(source);
				IVectorView<TimedMetadataTrack> tracks = media_item.TimedMetadataTracks();

				for (size_t index = 0; index < tracks.Size(); index++) {
					TimedMetadataTrack track = tracks.GetAt(index);

					if (track.TimedMetadataKind() == TimedMetadataKind::Speech) {
						track.CueEntered([this, utterance_id](TimedMetadataTrack const &sender, MediaCueEventArgs const &args) {
							SpeechCue cue = args.Cue().as<SpeechCue>();
							if (cue != nullptr) {
								hstring cue_type = sender.Label();

								if (cue_type == L"SpeechWord") {
									int position_in_input = cue.StartPositionInInput().as<int>();
									DisplayServer::get_singleton()->tts_post_utterance_event(DisplayServer::TTS_UTTERANCE_BOUNDARY, utterance_id, position_in_input);
								}
							}
						});

						media_item.TimedMetadataTracks().SetPresentationMode(index, TimedMetadataTrackPresentationMode::Hidden);
					}
				}

				media_player.Source(media_item);
				media_player.Play();
				DisplayServer::get_singleton()->tts_post_utterance_event(DisplayServer::TTS_UTTERANCE_STARTED, pending_utterance_id);
			}
		});

		queue.pop_front();
	}
}

TTS_Windows *TTS_Windows::get_singleton() {
	return singleton;
}

bool TTS_Windows::is_ssml(DisplayServer::TTSUtterance &message) const {
	// Checks for the "<speak" tag at the start of the text, which should be true for all
	// valid SSML 1.0 strings. Doesn't check if the full string is valid SSML 1.0 according
	// to WinRT, so messages that pass this test can still fail to be synthesized.
	String text = message.text.strip_edges(true, false).substr(0, 6).to_lower();
	return text == "<speak";
}

TTS_Windows::TTS_Windows() {
	singleton = this;

	pending_utterance_id = NO_UTTERANCE_ID;

	synthesizer = SpeechSynthesizer();
	if (synthesizer == nullptr) {
		print_verbose("Text-to-Speech: Cannot initialize ISpeechSynthesizer!");
		return;
	}

	media_player = MediaPlayer();
	if (media_player == nullptr) {
		print_verbose("Text-to-Speech: Cannot initialize MediaPlayer!");
		return;
	}

	synthesizer.Options().IncludeWordBoundaryMetadata(true);
	synthesizer.Options().IncludeSentenceBoundaryMetadata(false);

	media_player.MediaEnded([this](MediaPlayer const &sender, IInspectable const &) {
		if (pending_utterance_id != NO_UTTERANCE_ID) {
			DisplayServer::get_singleton()->tts_post_utterance_event(DisplayServer::TTS_UTTERANCE_ENDED, pending_utterance_id);
			pending_utterance_id = NO_UTTERANCE_ID;
		}
	});
	media_player.MediaFailed([this](MediaPlayer const &sender, MediaPlayerFailedEventArgs const &args) {
		if (pending_utterance_id != NO_UTTERANCE_ID) {
			DisplayServer::get_singleton()->tts_post_utterance_event(DisplayServer::TTS_UTTERANCE_CANCELED, pending_utterance_id);
			pending_utterance_id = NO_UTTERANCE_ID;
		}
	});

	print_verbose("Text-to-Speech: ISpeechSynthesizer initialized.");
}

TTS_Windows::~TTS_Windows() {
	stop();
	media_player.Close();
	synthesizer.Close();
	singleton = nullptr;
}
