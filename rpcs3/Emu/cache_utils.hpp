#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>
#include "util/types.hpp"

namespace rpcs3::cache
{
	inline constexpr u32 emu_cache_schema_version = 1;
	inline constexpr u32 ppu_cache_schema_version = 1;
	inline constexpr u32 spu_cache_schema_version = 1;
	inline constexpr u32 rsx_cache_schema_version = 1;

	enum class cas_codec : u8
	{
		auto_select = 0,
		none = 1,
		lz4 = 2,
		zstd = 3,
	};

	enum class cas_cache_tier : u8
	{
		auto_select = 0,
		hot = 1,
		warm = 2,
		cold = 3,
	};

	enum class cas_artifact_type : u8
	{
		auto_select = 0,
		spu_function_blob = 1,
		ppu_object_blob = 2,
		rsx_raw_fp_blob = 3,
		rsx_raw_vp_blob = 4,
		rsx_pipeline_blob = 5,
	};

	struct manifest_record
	{
		std::string artifact_type;
		std::string hash_key;
		std::string metadata;
		std::string compatibility_tuple;
		std::string format_version;
		std::string codec;
		std::string tier;
	};

	struct run_info
	{
		std::string title_id;
		std::string run_id;
		u64 created_at = 0;
		u64 last_accessed_at = 0;
		bool pinned = false;
		std::string run_reason;
		std::string label;
	};

	enum class run_mismatch_reason : u8
	{
		title_id,
		emu_schema,
		rsx_schema,
		backend,
		gpu,
		settings,
	};

	struct run_mismatch
	{
		run_mismatch_reason reason{};
		bool hard_constraint = false;
		std::string detail;
	};

	struct run_match_options
	{
		std::string_view required_backend;
		u32 weight_gpu = 500;
		u32 weight_settings = 220;
		u32 weight_pinned = 30;
		u32 weight_recency = 20;
	};

	struct run_match_result
	{
		std::string run_id;
		s64 score = 0;
		bool full_reuse = false;
		bool partial_rebuild = false;
		std::vector<run_mismatch> mismatches;
	};

	std::string get_ppu_cache();
	std::string get_shared_cas_root();
	std::string get_platform_cache_id();
	using compatibility_field = std::pair<std::string_view, std::string_view>;
	std::string make_platform_fields(std::initializer_list<compatibility_field> fields);
	std::string make_rsx_platform_fields(std::string_view renderer_backend, std::initializer_list<compatibility_field> platform_fields = {});
	std::string make_compatibility_tuple(std::string_view domain, std::string_view backend_id, std::string_view platform_fields = {});
	std::string_view to_manifest_artifact_name(cas_artifact_type artifact);
	cas_cache_tier get_default_tier_for_artifact(cas_artifact_type artifact);

	std::string put_to_cas(const void* data, std::size_t size, cas_artifact_type artifact, cas_cache_tier tier = cas_cache_tier::auto_select);
	std::string put_to_cas(const void* data, std::size_t size, std::string_view extension = {}, cas_cache_tier tier = cas_cache_tier::auto_select, cas_codec codec = cas_codec::auto_select);
	bool write_file_atomic(const std::string& path, const void* data, std::size_t size);
	bool write_text_file_atomic(const std::string& path, std::string_view text);
	bool append_manifest_record_atomic(const std::string& path, std::string_view record, bool use_journal = true);
	bool get_from_cas(const std::string& hash_key, std::vector<uchar>& out);
	std::string make_manifest_record(cas_artifact_type artifact, const std::string& hash_key, std::string_view metadata = {}, std::string_view compatibility_tuple = {}, std::string_view format_version = {}, cas_cache_tier tier = cas_cache_tier::auto_select);
	std::string make_manifest_record(std::string_view artifact_type, const std::string& hash_key, std::string_view metadata = {}, std::string_view compatibility_tuple = {}, std::string_view format_version = {}, cas_codec codec = cas_codec::auto_select, cas_cache_tier tier = cas_cache_tier::auto_select);
	std::vector<run_info> list_runs(std::string_view title_id);
	run_match_result find_best_run_match(std::string_view title_id, std::string_view current_settings_fingerprint, std::string_view current_gpu_fingerprint, const run_match_options& options = {});
	bool set_run_pinned(std::string_view title_id, std::string_view run_id, bool pinned);
	bool set_current_run_pinned(bool pinned);
	bool parse_manifest_record(std::string_view line, manifest_record& out);
	bool is_manifest_record_compatible(const manifest_record& rec, cas_artifact_type expected_artifact, std::string_view expected_compatibility_tuple, std::string_view expected_format_version, cas_cache_tier expected_tier = cas_cache_tier::auto_select);
	bool is_manifest_record_compatible(const manifest_record& rec, std::string_view expected_artifact_type, std::string_view expected_compatibility_tuple, std::string_view expected_format_version, cas_codec expected_codec = cas_codec::auto_select, cas_cache_tier expected_tier = cas_cache_tier::auto_select);
	void record_catalog_reference(std::string_view family, std::string_view artifact_type, std::string_view hash_key, std::string_view compatibility_tuple, std::string_view format_version, std::string_view settings_fingerprint, std::string_view gpu_fingerprint = {});
	void run_policy_gc(bool hard_emergency_mode = false);
	void limit_cache_size();
}
