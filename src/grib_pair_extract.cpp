#include "avenc_version.h"
#include <eccodes.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct VariableState {
    bool kept_first = false;
    long first_level = std::numeric_limits<long>::min();
    long first_step = std::numeric_limits<long>::min();
    bool has_multi_level_or_step = false;
    std::uint64_t multi_counter = 0;
};

struct StreamPlan {
    std::string short_name;
    std::uint64_t total_messages = 0;
    std::uint64_t default_kept = 0;
    std::uint64_t target_kept = 0;
};

struct Options {
    std::uint64_t max_bin_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL; // 2 GiB per .bin file
    std::unordered_set<std::string> only_shortnames;
    std::string original_grib;
    std::string decompressed_grib;
    std::string original_bin_path;
    std::string decompressed_bin_path;
    std::string csv_path;
};

[[noreturn]] void usage(const char *prog) {
    std::cerr
        << "Usage: " << prog
        << " [--max-bin-bytes N] [--only-shortnames a,b,c] <original.grib> <decompressed.grib> <original_data.bin> <decompressed_data.bin> <index.csv>\n"
        << "  --max-bin-bytes N   Cap output size per .bin file (default: 2147483648)\n";
    std::cerr
        << "  --only-shortnames   Keep only listed variables (comma-separated), e.g. 10u,10v\n";
    std::exit(1);
}

std::string trim_copy(const std::string &s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::unordered_set<std::string> parse_shortname_list(const std::string &csv) {
    std::unordered_set<std::string> out;
    std::size_t start = 0;
    while (start <= csv.size()) {
        std::size_t pos = csv.find(',', start);
        std::string tok = (pos == std::string::npos) ? csv.substr(start) : csv.substr(start, pos - start);
        tok = trim_copy(tok);
        if (!tok.empty()) out.insert(tok);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}

std::string csv_escape(const std::string &v) {
    bool needs_quotes = false;
    for (char c : v) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) return v;
    std::string out;
    out.reserve(v.size() + 4);
    out.push_back('"');
    for (char c : v) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

long get_long_key(codes_handle *h, const char *key, long fallback = 0) {
    long v = fallback;
    if (codes_get_long(h, key, &v) != CODES_SUCCESS) {
        return fallback;
    }
    return v;
}

std::string get_string_key(codes_handle *h, const char *key) {
    size_t len = 0;
    if (codes_get_length(h, key, &len) != CODES_SUCCESS || len == 0) {
        return "";
    }
    std::vector<char> buf(len + 1, '\0');
    size_t out_len = buf.size();
    if (codes_get_string(h, key, buf.data(), &out_len) != CODES_SUCCESS) {
        return "";
    }
    if (out_len > 0 && buf[out_len - 1] == '\0') {
        return std::string(buf.data(), out_len - 1);
    }
    return std::string(buf.data(), out_len);
}

bool should_sample(const std::string &short_name,
                   long level,
                   long step,
                   std::unordered_map<std::string, VariableState> &states) {
    VariableState &st = states[short_name];

    // Keep at least one sample for each variable.
    if (!st.kept_first) {
        st.kept_first = true;
        st.first_level = level;
        st.first_step = step;
        return true;
    }

    if (level != st.first_level || step != st.first_step) {
        st.has_multi_level_or_step = true;
    }

    // If no level/step diversity observed for this variable, keep all messages.
    if (!st.has_multi_level_or_step) {
        return true;
    }

    // For multi-level or multi-step variables, keep about 10%.
    st.multi_counter += 1;
    return (st.multi_counter % 10ULL) == 0ULL;
}

bool keep_from_quota(std::uint64_t idx, std::uint64_t total, std::uint64_t keep) {
    if (keep == 0) return false;
    if (keep >= total) return true;
    // Always keep first kept candidate for each stream, then spread others.
    if (idx == 0) return true;
    if (keep == 1) return false;
    if (total <= 1) return true;
    const std::uint64_t lhs = (idx * (keep - 1)) / (total - 1);
    const std::uint64_t rhs = ((idx - 1) * (keep - 1)) / (total - 1);
    return lhs > rhs;
}

Options parse_args(int argc, char **argv) {
    Options opt;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-bin-bytes") {
            if (i + 1 >= argc) usage(argv[0]);
            try {
                opt.max_bin_bytes = static_cast<std::uint64_t>(std::stoull(argv[++i]));
            } catch (...) {
                std::cerr << "Invalid value for --max-bin-bytes: " << argv[i] << "\n";
                usage(argv[0]);
            }
            continue;
        }
        if (arg == "--only-shortnames") {
            if (i + 1 >= argc) usage(argv[0]);
            opt.only_shortnames = parse_shortname_list(argv[++i]);
            continue;
        }
        if (!arg.empty() && arg[0] == '-') usage(argv[0]);
        pos.push_back(arg);
    }
    if (pos.size() != 5) usage(argv[0]);
    opt.original_grib = pos[0];
    opt.decompressed_grib = pos[1];
    opt.original_bin_path = pos[2];
    opt.decompressed_bin_path = pos[3];
    opt.csv_path = pos[4];
    return opt;
}

std::unordered_map<std::string, StreamPlan> build_stream_plans(const std::string &original_grib,
                                                               std::uint64_t max_bin_bytes,
                                                               const std::unordered_set<std::string> &only_shortnames,
                                                               std::size_t &first_points,
                                                               std::uint64_t &default_kept_total) {
    std::unordered_map<std::string, StreamPlan> plans;
    std::vector<std::string> order;
    std::unordered_map<std::string, VariableState> states;
    first_points = 0;
    default_kept_total = 0;

    FILE *fp = std::fopen(original_grib.c_str(), "rb");
    if (!fp) {
        throw std::runtime_error("Error: cannot open " + original_grib + " for pre-scan");
    }

    while (true) {
        int err = 0;
        codes_handle *h = codes_handle_new_from_file(nullptr, fp, PRODUCT_GRIB, &err);
        if (!h) {
            if (err == CODES_SUCCESS || err == CODES_END_OF_FILE) break;
            std::fclose(fp);
            throw std::runtime_error("Error: failed while pre-scanning GRIB headers");
        }

        std::string short_name = get_string_key(h, "shortName");
        if (short_name.empty()) short_name = "unknown";
        if (!only_shortnames.empty() && only_shortnames.find(short_name) == only_shortnames.end()) {
            codes_handle_delete(h);
            continue;
        }
        const long level = get_long_key(h, "level", 0);
        const long step = get_long_key(h, "step", 0);

        auto it = plans.find(short_name);
        if (it == plans.end()) {
            StreamPlan sp;
            sp.short_name = short_name;
            plans.emplace(short_name, sp);
            order.push_back(short_name);
            it = plans.find(short_name);
        }
        StreamPlan &sp = it->second;
        sp.total_messages += 1;

        if (first_points == 0) {
            size_t nvals = 0;
            if (codes_get_size(h, "values", &nvals) == CODES_SUCCESS && nvals > 0) {
                first_points = nvals;
            }
        }

        const bool keep_default =
            (!only_shortnames.empty()) ? true : should_sample(short_name, level, step, states);
        if (keep_default) {
            sp.default_kept += 1;
            default_kept_total += 1;
        }

        codes_handle_delete(h);
    }
    std::fclose(fp);

    if (first_points == 0) {
        throw std::runtime_error("Error: unable to determine first-frame grid size during pre-scan");
    }

    // Convert bin-size cap to max kept message pairs using first frame size.
    const std::uint64_t bytes_per_pair_per_bin = static_cast<std::uint64_t>(first_points) * sizeof(float);
    if (bytes_per_pair_per_bin == 0) {
        throw std::runtime_error("Error: invalid zero bytes_per_pair in pre-scan");
    }
    std::uint64_t max_pairs = max_bin_bytes / bytes_per_pair_per_bin;
    if (max_pairs == 0) max_pairs = 1;

    // Default: keep the same strategy.
    for (auto &kv : plans) {
        kv.second.target_kept = kv.second.default_kept;
    }
    if (default_kept_total <= max_pairs) {
        return plans;
    }

    // Cap active: preserve at least one default-kept sample per stream when possible.
    std::uint64_t assigned = 0;
    std::vector<std::string> active_streams;
    active_streams.reserve(order.size());
    for (const auto &name : order) {
        auto itp = plans.find(name);
        if (itp == plans.end()) continue;
        StreamPlan &sp = itp->second;
        if (sp.default_kept == 0) continue;
        active_streams.push_back(name);
        sp.target_kept = 0;
    }

    const std::uint64_t base = std::min<std::uint64_t>(active_streams.size(), max_pairs);
    for (std::uint64_t i = 0; i < base; ++i) {
        plans[active_streams[static_cast<std::size_t>(i)]].target_kept = 1;
        assigned += 1;
    }

    if (assigned < max_pairs && !active_streams.empty()) {
        const std::uint64_t remaining = max_pairs - assigned;
        std::uint64_t total_extra = 0;
        for (const auto &name : active_streams) {
            const StreamPlan &sp = plans[name];
            if (sp.default_kept > sp.target_kept) total_extra += (sp.default_kept - sp.target_kept);
        }
        if (total_extra > 0) {
            struct Frac {
                std::string name;
                std::uint64_t rem = 0;
            };
            std::vector<Frac> fracs;
            fracs.reserve(active_streams.size());
            std::uint64_t used = 0;
            for (const auto &name : active_streams) {
                StreamPlan &sp = plans[name];
                const std::uint64_t extra_cap = (sp.default_kept > sp.target_kept) ? (sp.default_kept - sp.target_kept) : 0;
                if (extra_cap == 0) continue;
                const std::uint64_t num = remaining * extra_cap;
                const std::uint64_t add = num / total_extra;
                const std::uint64_t rem = num % total_extra;
                const std::uint64_t clamped = std::min<std::uint64_t>(add, extra_cap);
                sp.target_kept += clamped;
                used += clamped;
                fracs.push_back({name, rem});
            }
            std::uint64_t left = (remaining > used) ? (remaining - used) : 0;
            std::sort(fracs.begin(), fracs.end(), [](const Frac &a, const Frac &b) {
                return a.rem > b.rem;
            });
            for (const auto &f : fracs) {
                if (left == 0) break;
                StreamPlan &sp = plans[f.name];
                if (sp.target_kept < sp.default_kept) {
                    sp.target_kept += 1;
                    left -= 1;
                }
            }
        }
    }

    return plans;
}

} // namespace

int main(int argc, char **argv) {
    if (avenc_print_version(argc, argv)) return 0;
    const Options opts = parse_args(argc, argv);
    const std::string &original_grib = opts.original_grib;
    const std::string &decompressed_grib = opts.decompressed_grib;
    const std::string &original_bin_path = opts.original_bin_path;
    const std::string &decompressed_bin_path = opts.decompressed_bin_path;
    const std::string &csv_path = opts.csv_path;

    std::size_t first_points = 0;
    std::uint64_t default_kept_total = 0;
    std::unordered_map<std::string, StreamPlan> plans;
    try {
        plans = build_stream_plans(original_grib, opts.max_bin_bytes, opts.only_shortnames, first_points, default_kept_total);
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 2;
    }

    FILE *f_orig = std::fopen(original_grib.c_str(), "rb");
    if (!f_orig) {
        std::cerr << "Error: cannot open " << original_grib << "\n";
        return 2;
    }
    FILE *f_dec = std::fopen(decompressed_grib.c_str(), "rb");
    if (!f_dec) {
        std::cerr << "Error: cannot open " << decompressed_grib << "\n";
        std::fclose(f_orig);
        return 2;
    }

    std::ofstream out_orig(original_bin_path, std::ios::binary | std::ios::trunc);
    std::ofstream out_dec(decompressed_bin_path, std::ios::binary | std::ios::trunc);
    std::ofstream out_csv(csv_path, std::ios::out | std::ios::trunc);
    if (!out_orig.is_open() || !out_dec.is_open() || !out_csv.is_open()) {
        std::cerr << "Error: unable to open one or more output files\n";
        std::fclose(f_orig);
        std::fclose(f_dec);
        return 2;
    }

    out_csv << "shortName,levelType,level,validityDate,validityTime,step,numGridPoints,byteOffset\n";

    std::unordered_map<std::string, VariableState> states;
    std::unordered_map<std::string, std::uint64_t> default_seen_per_stream;
    std::uint64_t byte_offset = 0;
    std::uint64_t message_pairs = 0;
    std::uint64_t kept_pairs = 0;

    while (true) {
        int err_o = 0;
        int err_d = 0;
        codes_handle *h_o = codes_handle_new_from_file(nullptr, f_orig, PRODUCT_GRIB, &err_o);
        codes_handle *h_d = codes_handle_new_from_file(nullptr, f_dec, PRODUCT_GRIB, &err_d);

        if (!h_o && !h_d) {
            if ((err_o == CODES_SUCCESS || err_o == CODES_END_OF_FILE) &&
                (err_d == CODES_SUCCESS || err_d == CODES_END_OF_FILE)) {
                break;
            }
            std::cerr << "Error: failed to read GRIB message pair at EOF check (err_o="
                      << err_o << ", err_d=" << err_d << ")\n";
            std::fclose(f_orig);
            std::fclose(f_dec);
            return 3;
        }

        if (!h_o || !h_d) {
            if (h_o) codes_handle_delete(h_o);
            if (h_d) codes_handle_delete(h_d);
            std::cerr << "Error: files do not have the same number of messages (pair index="
                      << message_pairs << ")\n";
            std::fclose(f_orig);
            std::fclose(f_dec);
            return 4;
        }

        message_pairs += 1;

        std::string short_name = get_string_key(h_o, "shortName");
        std::string level_type = get_string_key(h_o, "typeOfLevel");
        long level = get_long_key(h_o, "level", 0);
        long validity_date = get_long_key(h_o, "validityDate", 0);
        long validity_time = get_long_key(h_o, "validityTime", 0);
        long step = get_long_key(h_o, "step", 0);

        if (short_name.empty()) short_name = "unknown";
        if (!opts.only_shortnames.empty() && opts.only_shortnames.find(short_name) == opts.only_shortnames.end()) {
            codes_handle_delete(h_o);
            codes_handle_delete(h_d);
            continue;
        }

        const bool keep_default =
            (!opts.only_shortnames.empty()) ? true : should_sample(short_name, level, step, states);
        if (!keep_default) {
            codes_handle_delete(h_o);
            codes_handle_delete(h_d);
            continue;
        }

        const auto it_plan = plans.find(short_name);
        std::uint64_t stream_default_total = 0;
        std::uint64_t stream_target_total = 0;
        if (it_plan != plans.end()) {
            stream_default_total = it_plan->second.default_kept;
            stream_target_total = it_plan->second.target_kept;
        } else {
            // Fallback (should not happen): keep default strategy.
            stream_default_total = 1;
            stream_target_total = 1;
        }

        const std::uint64_t idx_default = default_seen_per_stream[short_name];
        default_seen_per_stream[short_name] = idx_default + 1;
        const bool keep = keep_from_quota(idx_default, stream_default_total, stream_target_total);
        if (!keep) {
            codes_handle_delete(h_o);
            codes_handle_delete(h_d);
            continue;
        }

        size_t n_o = 0;
        size_t n_d = 0;
        if (codes_get_size(h_o, "values", &n_o) != CODES_SUCCESS ||
            codes_get_size(h_d, "values", &n_d) != CODES_SUCCESS) {
            codes_handle_delete(h_o);
            codes_handle_delete(h_d);
            std::cerr << "Error: unable to get values size at message pair " << message_pairs << "\n";
            return 5;
        }
        if (n_o == 0 || n_d == 0 || n_o != n_d) {
            codes_handle_delete(h_o);
            codes_handle_delete(h_d);
            std::cerr << "Error: values size mismatch at message pair " << message_pairs
                      << " (" << n_o << " vs " << n_d << ")\n";
            return 6;
        }

        double *vals_o = static_cast<double *>(std::malloc(n_o * sizeof(double)));
        double *vals_d = static_cast<double *>(std::malloc(n_o * sizeof(double)));
        float *fvals_o = static_cast<float *>(std::malloc(n_o * sizeof(float)));
        float *fvals_d = static_cast<float *>(std::malloc(n_o * sizeof(float)));
        if (!vals_o || !vals_d || !fvals_o || !fvals_d) {
            std::free(vals_o);
            std::free(vals_d);
            std::free(fvals_o);
            std::free(fvals_d);
            codes_handle_delete(h_o);
            codes_handle_delete(h_d);
            std::cerr << "Error: memory allocation failed at message pair " << message_pairs << "\n";
            return 7;
        }

        size_t tmp_n_o = n_o;
        size_t tmp_n_d = n_o;
        const int rc_o = codes_get_double_array(h_o, "values", vals_o, &tmp_n_o);
        const int rc_d = codes_get_double_array(h_d, "values", vals_d, &tmp_n_d);
        if (rc_o != CODES_SUCCESS || rc_d != CODES_SUCCESS || tmp_n_o != n_o || tmp_n_d != n_o) {
            std::free(vals_o);
            std::free(vals_d);
            std::free(fvals_o);
            std::free(fvals_d);
            codes_handle_delete(h_o);
            codes_handle_delete(h_d);
            std::cerr << "Error: unable to read values array at message pair " << message_pairs
                      << " (rc_o=" << rc_o << ", rc_d=" << rc_d << ")\n";
            return 8;
        }

        for (size_t i = 0; i < n_o; ++i) {
            fvals_o[i] = static_cast<float>(vals_o[i]);
            fvals_d[i] = static_cast<float>(vals_d[i]);
        }

        out_orig.write(reinterpret_cast<const char *>(fvals_o), static_cast<std::streamsize>(n_o * sizeof(float)));
        out_dec.write(reinterpret_cast<const char *>(fvals_d), static_cast<std::streamsize>(n_o * sizeof(float)));
        if (!out_orig.good() || !out_dec.good()) {
            std::free(vals_o);
            std::free(vals_d);
            std::free(fvals_o);
            std::free(fvals_d);
            codes_handle_delete(h_o);
            codes_handle_delete(h_d);
            std::cerr << "Error: failed writing binary outputs at message pair " << message_pairs << "\n";
            return 9;
        }

        out_csv << csv_escape(short_name) << ","
                << csv_escape(level_type) << ","
                << level << ","
                << validity_date << ","
                << validity_time << ","
                << step << ","
                << n_o << ","
                << byte_offset << "\n";
        if (!out_csv.good()) {
            std::free(vals_o);
            std::free(vals_d);
            std::free(fvals_o);
            std::free(fvals_d);
            codes_handle_delete(h_o);
            codes_handle_delete(h_d);
            std::cerr << "Error: failed writing CSV index at message pair " << message_pairs << "\n";
            return 10;
        }

        byte_offset += static_cast<std::uint64_t>(n_o * sizeof(float));
        kept_pairs += 1;

        std::free(vals_o);
        std::free(vals_d);
        std::free(fvals_o);
        std::free(fvals_d);
        codes_handle_delete(h_o);
        codes_handle_delete(h_d);
    }

    std::fclose(f_orig);
    std::fclose(f_dec);
    out_orig.close();
    out_dec.close();
    out_csv.close();

    const std::uint64_t bytes_per_pair_per_bin = static_cast<std::uint64_t>(first_points) * sizeof(float);
    std::uint64_t max_pairs = opts.max_bin_bytes / std::max<std::uint64_t>(1, bytes_per_pair_per_bin);
    if (max_pairs == 0) max_pairs = 1;

    std::cerr << "Done. Read pairs=" << message_pairs
              << ", kept=" << kept_pairs
              << ", default_kept=" << default_kept_total
              << ", cap_pairs=" << max_pairs
              << ", streams=" << plans.size()
              << ", bytes_per_bin=" << byte_offset << "\n";
    return 0;
}
