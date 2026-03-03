#include "stdafx.h"

#include "Emu/Audio/audio_runtime_config.h"

namespace
{
	constexpr s64 accurate_buffer_ms = 34;
	constexpr s64 accurate_time_stretch_threshold = 75;
	constexpr s64 low_latency_buffer_ms = 12;
	constexpr s64 low_latency_time_stretch_threshold = 50;
}

namespace audio
{
	runtime_resampler_params get_runtime_resampler_params(audio_profile profile)
	{
		switch (profile)
		{
		case audio_profile::accurate:
		case audio_profile::manual:
			return runtime_resampler_params{};
		case audio_profile::low_latency:
			return runtime_resampler_params{
				.sequence_ms = 20,
				.seekwindow_ms = 10,
				.overlap_ms = 6,
				.use_quickseek = 1,
				.use_aa_filter = 1,
			};
		}

		return runtime_resampler_params{};
	}

	resolved_runtime_profile resolve_runtime_profile(const runtime_profile_config& raw_cfg)
	{
		resolved_runtime_profile resolved{};
		resolved.buffering_enabled = raw_cfg.buffering_enabled;
		resolved.desired_buffer_duration = raw_cfg.desired_buffer_duration;
		resolved.enable_time_stretching = raw_cfg.enable_time_stretching;
		resolved.time_stretching_threshold = raw_cfg.time_stretching_threshold;
		resolved.disable_sampling_skip = raw_cfg.disable_sampling_skip;
		resolved.resampler = get_runtime_resampler_params(raw_cfg.profile);

		switch (raw_cfg.profile)
		{
		case audio_profile::manual:
			break;
		case audio_profile::accurate:
			resolved.applied_override = runtime_profile_override::accurate;
			resolved.buffering_enabled = true;
			resolved.desired_buffer_duration = accurate_buffer_ms;
			resolved.enable_time_stretching = false;
			resolved.time_stretching_threshold = accurate_time_stretch_threshold;
			resolved.disable_sampling_skip = false;
			break;
		case audio_profile::low_latency:
			resolved.applied_override = runtime_profile_override::low_latency;
			resolved.buffering_enabled = true;
			resolved.desired_buffer_duration = low_latency_buffer_ms;
			resolved.enable_time_stretching = true;
			resolved.time_stretching_threshold = low_latency_time_stretch_threshold;
			resolved.disable_sampling_skip = true;
			break;
		}

		return resolved;
	}
}
