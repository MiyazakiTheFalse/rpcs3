#include "stdafx.h"
#include "Emu/Audio/audio_resampler.h"
#include <algorithm>

audio_resampler::audio_resampler()
{
	set_preset(audio_profile::manual);
}

audio_resampler::~audio_resampler()
{
}

void audio_resampler::apply_params(const runtime_resampler_params& params)
{
	resampler.setSetting(SETTING_SEQUENCE_MS, params.sequence_ms);
	resampler.setSetting(SETTING_SEEKWINDOW_MS, params.seekwindow_ms);
	resampler.setSetting(SETTING_OVERLAP_MS, params.overlap_ms);
	resampler.setSetting(SETTING_USE_QUICKSEEK, params.use_quickseek);
	resampler.setSetting(SETTING_USE_AA_FILTER, params.use_aa_filter);
}

void audio_resampler::set_preset(audio_profile profile)
{
	m_profile = profile;
	apply_params(audio::get_runtime_resampler_params(m_profile));
}

void audio_resampler::set_params(AudioChannelCnt ch_cnt, AudioFreq freq)
{
	apply_params(audio::get_runtime_resampler_params(m_profile));
	flush();
	resampler.setChannels(static_cast<u32>(ch_cnt));
	resampler.setSampleRate(static_cast<u32>(freq));
}

f64 audio_resampler::set_tempo(f64 new_tempo)
{
	new_tempo = std::clamp(new_tempo, RESAMPLER_MIN_FREQ_VAL, RESAMPLER_MAX_FREQ_VAL);
	resampler.setTempo(new_tempo);
	return new_tempo;
}

void audio_resampler::put_samples(const f32* buf, u32 sample_cnt)
{
	resampler.putSamples(buf, sample_cnt);
}

std::pair<f32* /* buffer */, u32 /* samples */> audio_resampler::get_samples(u32 sample_cnt)
{
	// NOTE: Make sure to get the buffer first because receiveSamples advances its position internally
	//       and std::make_pair evaluates the second parameter first...
	f32* const buf = resampler.bufBegin();
	return std::make_pair(buf, resampler.receiveSamples(sample_cnt));
}

u32 audio_resampler::samples_available() const
{
	return resampler.numSamples();
}

f64 audio_resampler::get_resample_ratio()
{
	return resampler.getInputOutputSampleRatio();
}

void audio_resampler::flush()
{
	resampler.clear();
}
