#pragma once

#include "Emu/system_config_types.h"

#include <cstdint>

namespace audio
{
	struct runtime_profile_config
	{
		audio_profile profile = audio_profile::manual;
		bool buffering_enabled = false;
		s64 desired_buffer_duration = 0;
		bool enable_time_stretching = false;
		s64 time_stretching_threshold = 0;
		bool disable_sampling_skip = false;
	};

	enum class runtime_profile_override
	{
		manual,
		accurate,
		low_latency,
	};

	struct runtime_resampler_params
	{
		s32 sequence_ms = 40;
		s32 seekwindow_ms = 15;
		s32 overlap_ms = 8;
		s32 use_quickseek = 0;
		s32 use_aa_filter = 1;
	};

	struct resolved_runtime_profile
	{
		runtime_profile_override applied_override = runtime_profile_override::manual;
		bool buffering_enabled = false;
		s64 desired_buffer_duration = 0;
		bool enable_time_stretching = false;
		s64 time_stretching_threshold = 0;
		bool disable_sampling_skip = false;
		runtime_resampler_params resampler{};
	};

	resolved_runtime_profile resolve_runtime_profile(const runtime_profile_config& raw_cfg);

	runtime_resampler_params get_runtime_resampler_params(audio_profile profile);
}
