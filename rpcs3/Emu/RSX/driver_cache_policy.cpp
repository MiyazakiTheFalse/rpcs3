#include "driver_cache_policy.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace rsx::driver_cache_policy
{
	namespace
	{
		struct parsed_version
		{
			u32 major = 0;
			u32 minor = 0;
			u32 patch = 0;
			bool valid = false;
		};

		struct version_rule
		{
			std::string_view backend;
			std::string_view vendor;
			std::string_view family;
			u32 min_major = 0;
			u32 max_major = umax;
			reuse_policy policy = reuse_policy::allow;
			std::string_view reason;
		};

		static std::string normalize(std::string_view input)
		{
			std::string out(input);
			std::transform(out.begin(), out.end(), out.begin(), [](uchar c)
			{
				return static_cast<char>(std::tolower(c));
			});
			return out;
		}

		static parsed_version parse_version(std::string_view text)
		{
			parsed_version out{};
			u32* slots[3] = { &out.major, &out.minor, &out.patch };
			u32 slot = 0;

			for (usz i = 0; i < text.size() && slot < 3;)
			{
				if (!std::isdigit(static_cast<uchar>(text[i])))
				{
					++i;
					continue;
				}

				u32 value = 0;
				while (i < text.size() && std::isdigit(static_cast<uchar>(text[i])))
				{
					value = (value * 10) + static_cast<u32>(text[i] - '0');
					++i;
				}

				*slots[slot++] = value;
			}

			out.valid = slot > 0;
			return out;
		}

		static bool rule_matches(std::string_view actual, std::string_view expected)
		{
			return expected.empty() || actual == expected;
		}

		static constexpr std::array<version_rule, 4> s_rules =
		{{
			{"vulkan", "nvidia", "", 550, 559, reuse_policy::allow_with_pipeline_rebuild, "nvidia_550_targeted_pipeline_rebuild"},
			{"vulkan", "nvidia", "nvk", 0, umax, reuse_policy::allow_with_pipeline_rebuild, "nvk_pipeline_rebuild"},
			{"opengl", "mesa", "", 23, 23, reuse_policy::allow_with_pipeline_rebuild, "mesa_23_targeted_pipeline_rebuild"},
			{"opengl", "ati technologies inc.", "", 0, umax, reuse_policy::deny, "legacy_opengl_ati_deny"},
		}};
	}

	policy_decision evaluate_transition(const driver_identity& previous, const driver_identity& current)
	{
		const std::string backend = normalize(current.backend);
		const std::string previous_vendor = normalize(previous.vendor);
		const std::string current_vendor = normalize(current.vendor);
		const std::string current_family = normalize(current.family);
		const std::string previous_family = normalize(previous.family);

		if (backend.empty() || current_vendor.empty())
		{
			return { reuse_policy::allow_with_pipeline_rebuild, "insufficient_driver_identity" };
		}

		if (!previous_vendor.empty() && previous_vendor != current_vendor)
		{
			return { reuse_policy::deny, "vendor_change_deny" };
		}

		if (!previous_family.empty() && !current_family.empty() && previous_family != current_family)
		{
			return { reuse_policy::allow_with_pipeline_rebuild, "family_change_pipeline_rebuild" };
		}

		const auto prev_ver = parse_version(previous.version);
		const auto cur_ver = parse_version(current.version);
		if (!prev_ver.valid || !cur_ver.valid)
		{
			return { reuse_policy::allow_with_pipeline_rebuild, "unparseable_driver_version" };
		}

		for (const auto& rule : s_rules)
		{
			if (!rule_matches(backend, rule.backend) || !rule_matches(current_vendor, rule.vendor) || !rule_matches(current_family, rule.family))
			{
				continue;
			}

			if (cur_ver.major >= rule.min_major && cur_ver.major <= rule.max_major)
			{
				return { rule.policy, std::string(rule.reason) };
			}
		}

		if (prev_ver.major != cur_ver.major)
		{
			return { reuse_policy::allow_with_pipeline_rebuild, "driver_major_change_pipeline_rebuild" };
		}

		return { reuse_policy::allow, "driver_safe_reuse" };
	}
}
