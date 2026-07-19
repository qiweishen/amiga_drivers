#include "core/config.hpp"

#include <algorithm>
#include <fstream>
#include <set>

#include "core/util.hpp"

namespace jai {

	using ordered_json = nlohmann::ordered_json;

	namespace {

		[[noreturn]] void fail(const std::string &path, const std::string &what) {
			throw ConfigError("config error at " + (path.empty() ? std::string("<root>") : path) + ": " + what);
		}

		size_t levenshtein(const std::string &a, const std::string &b) {
			std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
			for (size_t j = 0; j <= b.size(); ++j) {
				prev[j] = j;
			}
			for (size_t i = 1; i <= a.size(); ++i) {
				cur[0] = i;
				for (size_t j = 1; j <= b.size(); ++j) {
					size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
					cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
				}
				std::swap(prev, cur);
			}
			return prev[b.size()];
		}

		std::string suggest_key(const std::string &key, const std::vector<std::string> &allowed) {
			std::string best;
			size_t best_dist = key.size();
			for (const auto &candidate: allowed) {
				size_t d = levenshtein(key, candidate);
				if (d < best_dist && d <= 3) {
					best_dist = d;
					best = candidate;
				}
			}
			return best;
		}

		// Every object in the config must declare its allowed keys; anything else is fatal.
		void check_keys(const ordered_json &obj, const std::vector<std::string> &allowed, const std::string &path) {
			for (auto it = obj.begin(); it != obj.end(); ++it) {
				if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end()) {
					std::string msg = "unknown key \"" + it.key() + "\"";
					std::string hint = suggest_key(it.key(), allowed);
					if (!hint.empty()) {
						msg += " (did you mean \"" + hint + "\"?)";
					}
					fail(path, msg);
				}
			}
		}

		const ordered_json *find(const ordered_json &obj, const char *key) {
			auto it = obj.find(key);
			return it == obj.end() ? nullptr : &*it;
		}

		void expect_object(const ordered_json &v, const std::string &path) {
			if (!v.is_object()) {
				fail(path, "expected an object");
			}
		}

		std::string get_string(const ordered_json &v, const std::string &path) {
			if (!v.is_string()) {
				fail(path, "expected a string");
			}
			return v.get<std::string>();
		}

		bool get_bool(const ordered_json &v, const std::string &path) {
			if (!v.is_boolean()) {
				fail(path, "expected true/false");
			}
			return v.get<bool>();
		}

		double get_number(const ordered_json &v, const std::string &path) {
			if (!v.is_number()) {
				fail(path, "expected a number");
			}
			return v.get<double>();
		}

		uint64_t get_uint(const ordered_json &v, const std::string &path) {
			if (!v.is_number_integer() || v.is_number_float() ||
				(v.is_number_integer() && !v.is_number_unsigned() && v.get<int64_t>() < 0)) {
				fail(path, "expected a non-negative integer");
			}
			return v.get<uint64_t>();
		}

		double get_nonneg_number(const ordered_json &v, const std::string &path) {
			double d = get_number(v, path);
			if (d < 0) {
				fail(path, "must be >= 0");
			}
			return d;
		}

		std::string get_enum(const ordered_json &v, const std::vector<std::string> &allowed, const std::string &path) {
			std::string s = get_string(v, path);
			if (std::find(allowed.begin(), allowed.end(), s) == allowed.end()) {
				std::string msg = "invalid value \"" + s + "\"; allowed:";
				for (const auto &a: allowed) {
					msg += " \"" + a + "\"";
				}
				fail(path, msg);
			}
			return s;
		}

		// Deep merge: override wins; nested objects merge recursively.
		ordered_json deep_merge(const ordered_json &base, const ordered_json &override_v) {
			if (!base.is_object() || !override_v.is_object()) {
				return override_v;
			}
			ordered_json out = base;
			for (auto it = override_v.begin(); it != override_v.end(); ++it) {
				auto found = out.find(it.key());
				if (found != out.end() && found->is_object() && it->is_object()) {
					*found = deep_merge(*found, *it);
				} else {
					out[it.key()] = *it;
				}
			}
			return out;
		}

		LoggingConfig parse_logging(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j, { "level" }, path);
			LoggingConfig c;
			if (const auto *v = find(j, "level")) {
				bool ok = false;
				c.level = parse_log_level(get_string(*v, path + ".level"), &ok);
				if (!ok) {
					fail(path + ".level", "expected trace|debug|info|warn|error");
				}
			}
			return c;
		}

		StatsConfig parse_stats(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j, { "enabled", "interval_s" }, path);
			StatsConfig c;
			if (const auto *v = find(j, "enabled"))
				c.enabled = get_bool(*v, path + ".enabled");
			if (const auto *v = find(j, "interval_s")) {
				c.interval_s = get_nonneg_number(*v, path + ".interval_s");
				if (c.interval_s <= 0) {
					fail(path + ".interval_s", "must be > 0");
				}
			}
			return c;
		}

		AcquisitionLimits parse_acquisition(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j, { "max_frames", "max_duration_s" }, path);
			AcquisitionLimits c;
			if (const auto *v = find(j, "max_frames"))
				c.max_frames = get_uint(*v, path + ".max_frames");
			if (const auto *v = find(j, "max_duration_s"))
				c.max_duration_s = get_nonneg_number(*v, path + ".max_duration_s");
			return c;
		}

		PreflightConfig parse_preflight(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j, { "fail_on_error" }, path);
			PreflightConfig c;
			if (const auto *v = find(j, "fail_on_error"))
				c.fail_on_error = get_bool(*v, path + ".fail_on_error");
			return c;
		}

		PtpConfig parse_ptp(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j,
					   { "enabled", "feature_set", "enable_feature", "status_feature", "required_status", "sync_timeout_s",
						 "poll_interval_ms", "on_timeout", "offset_report_interval_s", "expect_tai_offset" },
					   path);
			PtpConfig c;
			if (const auto *v = find(j, "enabled"))
				c.enabled = get_bool(*v, path + ".enabled");
			if (const auto *v = find(j, "feature_set"))
				c.feature_set = get_enum(*v, { "auto", "gev_ieee1588", "sfnc_ptp", "explicit" }, path + ".feature_set");
			if (const auto *v = find(j, "enable_feature"))
				c.enable_feature = get_string(*v, path + ".enable_feature");
			if (const auto *v = find(j, "status_feature"))
				c.status_feature = get_string(*v, path + ".status_feature");
			if (const auto *v = find(j, "required_status"))
				c.required_status = get_string(*v, path + ".required_status");
			if (const auto *v = find(j, "sync_timeout_s"))
				c.sync_timeout_s = get_nonneg_number(*v, path + ".sync_timeout_s");
			if (const auto *v = find(j, "poll_interval_ms")) {
				c.poll_interval_ms = static_cast<uint32_t>(get_uint(*v, path + ".poll_interval_ms"));
				if (c.poll_interval_ms == 0) {
					fail(path + ".poll_interval_ms", "must be > 0");
				}
			}
			if (const auto *v = find(j, "on_timeout"))
				c.on_timeout = get_enum(*v, { "abort", "warn_continue" }, path + ".on_timeout");
			if (const auto *v = find(j, "offset_report_interval_s"))
				c.offset_report_interval_s = get_nonneg_number(*v, path + ".offset_report_interval_s");
			if (const auto *v = find(j, "expect_tai_offset"))
				c.expect_tai_offset = get_bool(*v, path + ".expect_tai_offset");
			if (c.feature_set == "explicit" && (c.enable_feature.empty() || c.status_feature.empty())) {
				fail(path, "feature_set \"explicit\" requires enable_feature and status_feature");
			}
			return c;
		}

		RecordingConfig parse_recording(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j,
					   { "output_dir", "session_name", "segment_size_gib", "record_align", "payload_crc", "queue_max_frames",
						 "queue_on_full", "on_buffer_error", "flush_interval_mb", "min_free_gib", "debug_slowdown_us" },
					   path);
			RecordingConfig c;
			if (const auto *v = find(j, "output_dir"))
				c.output_dir = get_string(*v, path + ".output_dir");
			if (const auto *v = find(j, "session_name"))
				c.session_name = get_string(*v, path + ".session_name");
			if (const auto *v = find(j, "segment_size_gib")) {
				c.segment_size_gib = get_nonneg_number(*v, path + ".segment_size_gib");
				if (c.segment_size_gib <= 0) {
					fail(path + ".segment_size_gib", "must be > 0");
				}
			}
			if (const auto *v = find(j, "record_align")) {
				c.record_align = static_cast<uint32_t>(get_uint(*v, path + ".record_align"));
				if (c.record_align == 0 || (c.record_align & (c.record_align - 1)) != 0) {
					fail(path + ".record_align", "must be a power of two (1 disables alignment)");
				}
			}
			if (const auto *v = find(j, "payload_crc"))
				c.payload_crc = get_enum(*v, { "none", "crc32c" }, path + ".payload_crc");
			if (const auto *v = find(j, "queue_max_frames")) {
				c.queue_max_frames = static_cast<uint32_t>(get_uint(*v, path + ".queue_max_frames"));
				if (c.queue_max_frames < 2) {
					fail(path + ".queue_max_frames", "must be >= 2");
				}
			}
			if (const auto *v = find(j, "queue_on_full"))
				c.queue_on_full = get_enum(*v, { "drop_newest", "block" }, path + ".queue_on_full");
			if (const auto *v = find(j, "on_buffer_error"))
				c.on_buffer_error = get_enum(*v, { "record_flagged", "drop" }, path + ".on_buffer_error");
			if (const auto *v = find(j, "flush_interval_mb")) {
				c.flush_interval_mb = static_cast<uint32_t>(get_uint(*v, path + ".flush_interval_mb"));
				if (c.flush_interval_mb == 0) {
					fail(path + ".flush_interval_mb", "must be > 0");
				}
			}
			if (const auto *v = find(j, "min_free_gib"))
				c.min_free_gib = get_nonneg_number(*v, path + ".min_free_gib");
			if (const auto *v = find(j, "debug_slowdown_us"))
				c.debug_slowdown_us = static_cast<uint32_t>(get_uint(*v, path + ".debug_slowdown_us"));
			return c;
		}

		SelectorConfig parse_selector(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j, { "by", "value" }, path);
			SelectorConfig c;
			const auto *by = find(j, "by");
			const auto *value = find(j, "value");
			if (!by || !value) {
				fail(path, "selector requires \"by\" and \"value\"");
			}
			c.by = get_enum(*by, { "mac", "ip", "serial", "user_defined_name" }, path + ".by");
			c.value = get_string(*value, path + ".value");
			if (c.value.empty()) {
				fail(path + ".value", "must not be empty");
			}
			return c;
		}

		DiscoveryConfig parse_discovery(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j, { "timeout_ms", "retries", "retry_interval_ms", "force_ip" }, path);
			DiscoveryConfig c;
			if (const auto *v = find(j, "timeout_ms"))
				c.timeout_ms = static_cast<uint32_t>(get_uint(*v, path + ".timeout_ms"));
			if (const auto *v = find(j, "retries"))
				c.retries = static_cast<uint32_t>(get_uint(*v, path + ".retries"));
			if (const auto *v = find(j, "retry_interval_ms"))
				c.retry_interval_ms = static_cast<uint32_t>(get_uint(*v, path + ".retry_interval_ms"));
			if (const auto *v = find(j, "force_ip")) {
				const std::string fp = path + ".force_ip";
				expect_object(*v, fp);
				check_keys(*v, { "enabled", "ip", "subnet_mask", "gateway" }, fp);
				if (const auto *w = find(*v, "enabled"))
					c.force_ip.enabled = get_bool(*w, fp + ".enabled");
				if (const auto *w = find(*v, "ip"))
					c.force_ip.ip = get_string(*w, fp + ".ip");
				if (const auto *w = find(*v, "subnet_mask"))
					c.force_ip.subnet_mask = get_string(*w, fp + ".subnet_mask");
				if (const auto *w = find(*v, "gateway"))
					c.force_ip.gateway = get_string(*w, fp + ".gateway");
				if (c.force_ip.enabled && (c.force_ip.ip.empty() || c.force_ip.subnet_mask.empty())) {
					fail(fp, "force_ip.enabled requires ip and subnet_mask");
				}
			}
			return c;
		}

		ConvenienceConfig parse_convenience(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j, { "exposure_us", "gain", "frame_rate", "pixel_format", "roi", "trigger" }, path);
			ConvenienceConfig c;
			if (const auto *v = find(j, "exposure_us"))
				c.exposure_us = get_nonneg_number(*v, path + ".exposure_us");
			if (const auto *v = find(j, "gain"))
				c.gain = get_number(*v, path + ".gain");
			if (const auto *v = find(j, "frame_rate"))
				c.frame_rate = get_nonneg_number(*v, path + ".frame_rate");
			if (const auto *v = find(j, "pixel_format"))
				c.pixel_format = get_string(*v, path + ".pixel_format");
			if (const auto *v = find(j, "roi")) {
				const std::string rp = path + ".roi";
				expect_object(*v, rp);
				check_keys(*v, { "width", "height", "offset_x", "offset_y" }, rp);
				RoiConfig roi;
				if (const auto *w = find(*v, "width"))
					roi.width = static_cast<uint32_t>(get_uint(*w, rp + ".width"));
				if (const auto *w = find(*v, "height"))
					roi.height = static_cast<uint32_t>(get_uint(*w, rp + ".height"));
				if (const auto *w = find(*v, "offset_x"))
					roi.offset_x = static_cast<uint32_t>(get_uint(*w, rp + ".offset_x"));
				if (const auto *w = find(*v, "offset_y"))
					roi.offset_y = static_cast<uint32_t>(get_uint(*w, rp + ".offset_y"));
				c.roi = roi;
			}
			if (const auto *v = find(j, "trigger")) {
				const std::string tp = path + ".trigger";
				expect_object(*v, tp);
				check_keys(*v, { "enabled", "selector", "source", "activation" }, tp);
				TriggerConfig t;
				if (const auto *w = find(*v, "enabled"))
					t.enabled = get_bool(*w, tp + ".enabled");
				if (const auto *w = find(*v, "selector"))
					t.selector = get_string(*w, tp + ".selector");
				if (const auto *w = find(*v, "source"))
					t.source = get_string(*w, tp + ".source");
				if (const auto *w = find(*v, "activation"))
					t.activation = get_string(*w, tp + ".activation");
				c.trigger = t;
			}
			return c;
		}

		std::vector<GenicamFeature> parse_feature_list(const ordered_json &j, const std::string &path) {
			if (!j.is_array()) {
				fail(path, "expected an array of {feature, value, on_error?} objects");
			}
			std::vector<GenicamFeature> out;
			size_t idx = 0;
			for (const auto &item: j) {
				const std::string ip = path + "[" + std::to_string(idx) + "]";
				expect_object(item, ip);
				check_keys(item, { "feature", "value", "on_error" }, ip);
				GenicamFeature f;
				const auto *name = find(item, "feature");
				const auto *value = find(item, "value");
				if (!name || !value) {
					fail(ip, "requires \"feature\" and \"value\"");
				}
				f.feature = get_string(*name, ip + ".feature");
				if (f.feature.empty()) {
					fail(ip + ".feature", "must not be empty");
				}
				if (value->is_string()) {
					f.value = value->get<std::string>();
					f.value_is_string = true;
				} else if (value->is_boolean()) {
					f.value = value->get<bool>() ? "true" : "false";
				} else if (value->is_number()) {
					f.value = value->dump();
				} else if (value->is_null()) {
					f.value = "";  // command features: value ignored
				} else {
					fail(ip + ".value", "expected a scalar (string/number/bool/null)");
				}
				if (const auto *v = find(item, "on_error")) {
					f.on_error = get_enum(*v, { "fail", "warn", "skip" }, ip + ".on_error");
				}
				out.push_back(std::move(f));
				++idx;
			}
			return out;
		}

		ApplyConfig parse_apply(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j, { "verify_readback", "float_verify_tolerance_rel", "on_error_default" }, path);
			ApplyConfig c;
			if (const auto *v = find(j, "verify_readback"))
				c.verify_readback = get_bool(*v, path + ".verify_readback");
			if (const auto *v = find(j, "float_verify_tolerance_rel"))
				c.float_verify_tolerance_rel = get_nonneg_number(*v, path + ".float_verify_tolerance_rel");
			if (const auto *v = find(j, "on_error_default"))
				c.on_error_default = get_enum(*v, { "fail", "warn", "skip" }, path + ".on_error_default");
			return c;
		}

		StreamConfig parse_stream(const ordered_json &j, const std::string &path) {
			expect_object(j, path);
			check_keys(j,
					   { "channel", "buffer_count", "packet_size", "socket_rx_buffer_mib", "local_ip", "gev_scpd_ticks",
						 "receiver_tuning" },
					   path);
			StreamConfig c;
			if (const auto *v = find(j, "channel"))
				c.channel = static_cast<uint32_t>(get_uint(*v, path + ".channel"));
			if (const auto *v = find(j, "buffer_count")) {
				c.buffer_count = static_cast<uint32_t>(get_uint(*v, path + ".buffer_count"));
				if (c.buffer_count != 0 && c.buffer_count < 4) {
					fail(path + ".buffer_count", "must be 0 (auto) or >= 4");
				}
			}
			if (const auto *v = find(j, "packet_size")) {
				c.packet_size = static_cast<uint32_t>(get_uint(*v, path + ".packet_size"));
				if (c.packet_size != 0 && (c.packet_size < 576 || c.packet_size > 16000)) {
					fail(path + ".packet_size", "must be 0 (auto) or in [576, 16000]");
				}
			}
			if (const auto *v = find(j, "socket_rx_buffer_mib")) {
				c.socket_rx_buffer_mib = static_cast<uint32_t>(get_uint(*v, path + ".socket_rx_buffer_mib"));
				if (c.socket_rx_buffer_mib == 0) {
					fail(path + ".socket_rx_buffer_mib", "must be > 0");
				}
			}
			if (const auto *v = find(j, "local_ip"))
				c.local_ip = get_string(*v, path + ".local_ip");
			if (const auto *v = find(j, "gev_scpd_ticks"))
				c.gev_scpd_ticks = static_cast<uint32_t>(get_uint(*v, path + ".gev_scpd_ticks"));
			if (const auto *v = find(j, "receiver_tuning"))
				c.receiver_tuning = parse_feature_list(*v, path + ".receiver_tuning");
			return c;
		}

		CameraConfig parse_camera(const ordered_json &j, const ordered_json &global_ptp, const ordered_json &global_recording,
								  const std::string &path) {
			expect_object(j, path);
			check_keys(j,
					   { "id", "enabled", "selector", "discovery", "convenience", "genicam_features", "apply", "stream", "ptp",
						 "recording" },
					   path);
			CameraConfig c;
			const auto *id = find(j, "id");
			if (!id) {
				fail(path, "camera requires \"id\"");
			}
			c.id = get_string(*id, path + ".id");
			if (c.id.empty() || c.id.size() > 63) {
				fail(path + ".id", "must be 1..63 characters");
			}
			for (char ch: c.id) {
				if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) {
					fail(path + ".id", "only [A-Za-z0-9_-] allowed (used as directory name)");
				}
			}
			if (const auto *v = find(j, "enabled"))
				c.enabled = get_bool(*v, path + ".enabled");
			const auto *sel = find(j, "selector");
			if (!sel) {
				fail(path, "camera requires \"selector\"");
			}
			c.selector = parse_selector(*sel, path + ".selector");
			if (c.selector.by == "mac" && normalize_mac(c.selector.value).empty()) {
				fail(path + ".selector.value", "not a valid MAC address");
			}
			if (const auto *v = find(j, "discovery"))
				c.discovery = parse_discovery(*v, path + ".discovery");
			if (const auto *v = find(j, "convenience"))
				c.convenience = parse_convenience(*v, path + ".convenience");
			if (const auto *v = find(j, "genicam_features"))
				c.genicam_features = parse_feature_list(*v, path + ".genicam_features");
			if (const auto *v = find(j, "apply"))
				c.apply = parse_apply(*v, path + ".apply");
			if (const auto *v = find(j, "stream"))
				c.stream = parse_stream(*v, path + ".stream");

			// Per-camera ptp/recording: deep-merge onto the global objects so the
			// camera override only needs the keys it changes.
			ordered_json ptp_json = global_ptp;
			if (const auto *v = find(j, "ptp")) {
				expect_object(*v, path + ".ptp");
				ptp_json = deep_merge(global_ptp, *v);
			}
			c.ptp = parse_ptp(ptp_json, path + ".ptp");
			ordered_json rec_json = global_recording;
			if (const auto *v = find(j, "recording")) {
				expect_object(*v, path + ".recording");
				rec_json = deep_merge(global_recording, *v);
			}
			c.recording = parse_recording(rec_json, path + ".recording");
			return c;
		}

	}  // namespace

	AppConfig load_config_json(const ordered_json &doc) {
		if (!doc.is_object()) {
			throw ConfigError("config error: top level must be a JSON object");
		}
		check_keys(doc, { "version", "logging", "stats", "acquisition", "preflight", "ptp", "recording", "cameras" }, "");
		AppConfig cfg;
		cfg.raw = doc;

		const auto *version = find(doc, "version");
		if (!version) {
			fail("", "missing required key \"version\"");
		}
		if (!version->is_number_integer() || version->get<int64_t>() != 1) {
			fail("version", "this driver supports config version 1 only");
		}
		cfg.version = 1;

		if (const auto *v = find(doc, "logging"))
			cfg.logging = parse_logging(*v, "logging");
		if (const auto *v = find(doc, "stats"))
			cfg.stats = parse_stats(*v, "stats");
		if (const auto *v = find(doc, "acquisition"))
			cfg.acquisition = parse_acquisition(*v, "acquisition");
		if (const auto *v = find(doc, "preflight"))
			cfg.preflight = parse_preflight(*v, "preflight");

		ordered_json global_ptp = ordered_json::object();
		if (const auto *v = find(doc, "ptp")) {
			expect_object(*v, "ptp");
			global_ptp = *v;
		}
		cfg.ptp = parse_ptp(global_ptp, "ptp");

		ordered_json global_recording = ordered_json::object();
		if (const auto *v = find(doc, "recording")) {
			expect_object(*v, "recording");
			global_recording = *v;
		}
		cfg.recording = parse_recording(global_recording, "recording");

		const auto *cameras = find(doc, "cameras");
		if (!cameras || !cameras->is_array() || cameras->empty()) {
			fail("cameras", "must be a non-empty array");
		}
		size_t idx = 0;
		for (const auto &cam: *cameras) {
			cfg.cameras.push_back(parse_camera(cam, global_ptp, global_recording, "cameras[" + std::to_string(idx) + "]"));
			++idx;
		}

		std::set<std::string> ids;
		size_t enabled_count = 0;
		for (const auto &cam: cfg.cameras) {
			if (!ids.insert(cam.id).second) {
				fail("cameras", "duplicate camera id \"" + cam.id + "\"");
			}
			if (cam.enabled) {
				++enabled_count;
			}
		}
		if (enabled_count == 0) {
			fail("cameras", "at least one camera must be enabled");
		}
		return cfg;
	}

	AppConfig load_config(const std::string &path) {
		std::ifstream in(path);
		if (!in.is_open()) {
			throw ConfigError("config error: cannot open \"" + path + "\"");
		}
		ordered_json doc;
		try {
			doc = ordered_json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
		} catch (const nlohmann::json::parse_error &e) {
			throw ConfigError(std::string("config error: JSON parse failed: ") + e.what());
		}
		return load_config_json(doc);
	}

}  // namespace jai
