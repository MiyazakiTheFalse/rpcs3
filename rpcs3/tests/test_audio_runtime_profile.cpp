#include <gtest/gtest.h>

#include "Emu/Audio/audio_runtime_config.h"

TEST(audio_runtime_profile, manual_profile_preserves_user_values)
{
	const audio::runtime_profile_config raw{
		.profile = audio_profile::manual,
		.buffering_enabled = false,
		.desired_buffer_duration = 80,
		.enable_time_stretching = true,
		.time_stretching_threshold = 60,
		.disable_sampling_skip = true,
	};

	const auto resolved = audio::resolve_runtime_profile(raw);

	EXPECT_EQ(resolved.applied_override, audio::runtime_profile_override::manual);
	EXPECT_EQ(resolved.buffering_enabled, raw.buffering_enabled);
	EXPECT_EQ(resolved.desired_buffer_duration, raw.desired_buffer_duration);
	EXPECT_EQ(resolved.enable_time_stretching, raw.enable_time_stretching);
	EXPECT_EQ(resolved.time_stretching_threshold, raw.time_stretching_threshold);
	EXPECT_EQ(resolved.disable_sampling_skip, raw.disable_sampling_skip);
}

TEST(audio_runtime_profile, profiles_override_runtime_values)
{
	const audio::runtime_profile_config raw{
		.profile = audio_profile::manual,
		.buffering_enabled = false,
		.desired_buffer_duration = 100,
		.enable_time_stretching = true,
		.time_stretching_threshold = 10,
		.disable_sampling_skip = true,
	};

	const auto accurate = audio::resolve_runtime_profile({ .profile = audio_profile::accurate, .buffering_enabled = raw.buffering_enabled,
		.desired_buffer_duration = raw.desired_buffer_duration, .enable_time_stretching = raw.enable_time_stretching,
		.time_stretching_threshold = raw.time_stretching_threshold, .disable_sampling_skip = raw.disable_sampling_skip });
	const auto low_latency = audio::resolve_runtime_profile({ .profile = audio_profile::low_latency, .buffering_enabled = raw.buffering_enabled,
		.desired_buffer_duration = raw.desired_buffer_duration, .enable_time_stretching = raw.enable_time_stretching,
		.time_stretching_threshold = raw.time_stretching_threshold, .disable_sampling_skip = raw.disable_sampling_skip });

	EXPECT_EQ(accurate.applied_override, audio::runtime_profile_override::accurate);
	EXPECT_TRUE(accurate.buffering_enabled);
	EXPECT_FALSE(accurate.enable_time_stretching);
	EXPECT_EQ(accurate.desired_buffer_duration, 34);
	EXPECT_FALSE(accurate.disable_sampling_skip);

	EXPECT_EQ(low_latency.applied_override, audio::runtime_profile_override::low_latency);
	EXPECT_TRUE(low_latency.buffering_enabled);
	EXPECT_TRUE(low_latency.enable_time_stretching);
	EXPECT_EQ(low_latency.desired_buffer_duration, 12);
	EXPECT_TRUE(low_latency.disable_sampling_skip);

	EXPECT_NE(low_latency.desired_buffer_duration, accurate.desired_buffer_duration);
	EXPECT_NE(low_latency.enable_time_stretching, accurate.enable_time_stretching);
}

TEST(audio_runtime_profile, profile_changes_resampler_params)
{
	const auto accurate = audio::get_runtime_resampler_params(audio_profile::accurate);
	const auto low_latency = audio::get_runtime_resampler_params(audio_profile::low_latency);

	EXPECT_GT(accurate.sequence_ms, low_latency.sequence_ms);
	EXPECT_GT(accurate.seekwindow_ms, low_latency.seekwindow_ms);
	EXPECT_EQ(accurate.use_quickseek, 0);
	EXPECT_EQ(low_latency.use_quickseek, 1);
}
