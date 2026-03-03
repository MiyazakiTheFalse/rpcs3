#pragma once

#include <string>
#include <string_view>

#include "util/types.hpp"

namespace rsx::driver_cache_policy
{
	enum class reuse_policy
	{
		allow,
		allow_with_pipeline_rebuild,
		deny,
	};

	struct driver_identity
	{
		std::string backend;
		std::string vendor;
		std::string family;
		std::string version;
	};

	struct policy_decision
	{
		reuse_policy policy = reuse_policy::allow_with_pipeline_rebuild;
		std::string rationale;
	};

	policy_decision evaluate_transition(const driver_identity& previous, const driver_identity& current);
}
