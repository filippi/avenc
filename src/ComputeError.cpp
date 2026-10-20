#include "avenc_version.h"
#include <eccodes.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct IdxRow {
    std::uint64_t byte_offset = 0;
    std::size_t npoints = 0;
};

struct MsgMetric {
    std::size_t msg_index = 0;
    std::string short_name;
    long level = 0;
    long validity_date = 0;
    long validity_time = 0;
    long step = 0;
    double mae = 0.0;
    double rmse = 0.0;
    double nmae = 0.0;
    double nrmse = 0.0;
    double max_abs = 0.0;
    std::size_t npoints = 0;
};

struct VarAgg {
    std::uint64_t n = 0;
    long double sum_sq_err = 0.0L;
    long double sum_ref = 0.0L;
    long double sum_ref_sq = 0.0L;
    long double sum_val = 0.0L;
    long double sum_val_sq = 0.0L;
    double vmin = std::numeric_limits<double>::infinity();
    double vmax = -std::numeric_limits<double>::infinity();
};

struct VarMetric {
    std::string short_name;
    std::uint64_t n = 0;
    double rmse = 0.0;
    double nrmse = 0.0;
    double rel_rmse = 0.0;
    double value_range = 0.0;
    double var_ref = 0.0;
    double var_val = 0.0;
    double var_ratio = 0.0;
    double var_rel_error = 0.0;
};

struct Options {
    std::string grib_path;
    std::string raw_path;
    std::string idx_path;
    std::string output_path;
    int top_n = 10;
};

[[noreturn]] void usage(const char *prog) {
    std::cerr
        << "Usage: " << prog << " <input.grib> <reference.raw.bin> [options]\n"
        << "Options:\n"
        << "  --idx <raw.idx.csv>     Optional raw index with byteOffset/numPoints\n"
        << "  --output <file.txt>     Optional text report output\n"
        << "  --top <N>               Number of worst messages to list (default: 10)\n";
    std::exit(1);
}

std::vector<std::string> split_csv(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                cur.push_back(c);
            }
        } else {
            if (c == ',') {
                out.push_back(cur);
                cur.clear();
            } else if (c == '"') {
                in_quotes = true;
            } else {
                cur.push_back(c);
            }
        }
    }
    out.push_back(cur);
    return out;
}

Options parse_args(int argc, char **argv) {
    if (argc < 3) usage(argv[0]);
    Options opt;
    opt.grib_path = argv[1];
    opt.raw_path = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto need_val = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                usage(argv[0]);
            }
            return argv[++i];
        };
        if (a == "--idx") {
            opt.idx_path = need_val("--idx");
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
        } else if (a == "--output") {
            opt.output_path = need_val("--output");
        } else if (a == "--top") {
            opt.top_n = std::stoi(need_val("--top"));
        } else {
            std::cerr << "Unknown option: " << a << "\n";
            usage(argv[0]);
        }
    }
    if (opt.top_n <= 0) opt.top_n = 10;
    return opt;
}

long get_long_key(codes_handle *h, const char *key, long fallback = 0) {
    long v = fallback;
    if (codes_get_long(h, key, &v) != CODES_SUCCESS) return fallback;
    return v;
}

std::string get_string_key(codes_handle *h, const char *key) {
    size_t len = 0;
    if (codes_get_length(h, key, &len) != CODES_SUCCESS || len == 0) return "";
    std::vector<char> buf(len + 1, '\0');
    size_t out_len = buf.size();
    if (codes_get_string(h, key, buf.data(), &out_len) != CODES_SUCCESS) return "";
    if (out_len > 0 && buf[out_len - 1] == '\0') return std::string(buf.data(), out_len - 1);
    return std::string(buf.data(), out_len);
}

std::vector<IdxRow> read_idx(const std::string &path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("Cannot open idx file: " + path);
    std::string header;
    if (!std::getline(in, header)) throw std::runtime_error("Empty idx file: " + path);
    auto cols = split_csv(header);
    int c_off = -1, c_np = -1;
    for (int i = 0; i < static_cast<int>(cols.size()); ++i) {
        if (cols[i] == "byteOffset") c_off = i;
        if (cols[i] == "numPoints") c_np = i;
    }
    if (c_off < 0 || c_np < 0) throw std::runtime_error("idx file missing byteOffset/numPoints columns");

    std::vector<IdxRow> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto v = split_csv(line);
        if (c_off >= static_cast<int>(v.size()) || c_np >= static_cast<int>(v.size())) continue;
        IdxRow r;
        r.byte_offset = static_cast<std::uint64_t>(std::stoull(v[c_off]));
        r.npoints = static_cast<std::size_t>(std::stoull(v[c_np]));
        out.push_back(r);
    }
    return out;
}

std::string fmt(double v) {
    std::ostringstream oss;
    oss << std::setprecision(10) << v;
    return oss.str();
}

}  // namespace

int main(int argc, char **argv) {
    if (avenc_print_version(argc, argv)) return 0;
    try {
        const Options opt = parse_args(argc, argv);

        FILE *fg = std::fopen(opt.grib_path.c_str(), "rb");
        if (!fg) throw std::runtime_error("Cannot open GRIB: " + opt.grib_path);
        std::ifstream raw(opt.raw_path, std::ios::binary);
        if (!raw.is_open()) {
            std::fclose(fg);
            throw std::runtime_error("Cannot open raw file: " + opt.raw_path);
        }

        std::vector<IdxRow> idx;
        if (!opt.idx_path.empty()) idx = read_idx(opt.idx_path);

        long double g_abs = 0.0L, g_sq = 0.0L, g_bias = 0.0L;
        long double g_abs_norm = 0.0L, g_sq_norm = 0.0L;
        long double g_ref_sum = 0.0L, g_ref_sq = 0.0L;
        long double g_val_sum = 0.0L, g_val_sq = 0.0L;
        std::uint64_t g_n = 0;
        double global_max_abs = -1.0;
        std::size_t global_max_msg = 0;
        std::size_t global_max_point = 0;
        double global_max_ref = 0.0;
        double global_max_val = 0.0;

        std::vector<MsgMetric> metrics;
        std::map<std::string, VarAgg> var_aggs;
        std::uint64_t seq_offset = 0;
        std::size_t msg_idx = 0;

        while (true) {
            int err = 0;
            codes_handle *h = codes_handle_new_from_file(nullptr, fg, PRODUCT_GRIB, &err);
            if (!h) {
                if (err == CODES_SUCCESS || err == CODES_END_OF_FILE) break;
                std::fclose(fg);
                throw std::runtime_error("Failed reading GRIB message");
            }

            size_t nvals = 0;
            if (codes_get_size(h, "values", &nvals) != CODES_SUCCESS || nvals == 0) {
                codes_handle_delete(h);
                continue;
            }

            std::vector<double> vals(nvals, 0.0);
            size_t tmp = nvals;
            if (codes_get_double_array(h, "values", vals.data(), &tmp) != CODES_SUCCESS || tmp != nvals) {
                codes_handle_delete(h);
                throw std::runtime_error("Failed reading GRIB values array");
            }

            std::uint64_t off = seq_offset;
            if (!idx.empty()) {
                if (msg_idx >= idx.size()) {
                    codes_handle_delete(h);
                    throw std::runtime_error("idx rows shorter than GRIB message count");
                }
                off = idx[msg_idx].byte_offset;
                if (idx[msg_idx].npoints != nvals) {
                    codes_handle_delete(h);
                    throw std::runtime_error("idx numPoints mismatch at message " + std::to_string(msg_idx));
                }
            }
            raw.seekg(static_cast<std::streamoff>(off), std::ios::beg);
            if (!raw.good()) {
                codes_handle_delete(h);
                throw std::runtime_error("Failed seek in raw file at message " + std::to_string(msg_idx));
            }
            std::vector<float> ref(nvals, 0.0f);
            raw.read(reinterpret_cast<char *>(ref.data()), static_cast<std::streamsize>(nvals * sizeof(float)));
            if (raw.gcount() != static_cast<std::streamsize>(nvals * sizeof(float))) {
                codes_handle_delete(h);
                throw std::runtime_error("Raw read short at message " + std::to_string(msg_idx));
            }
            seq_offset += static_cast<std::uint64_t>(nvals * sizeof(float));

            MsgMetric mm;
            mm.msg_index = msg_idx;
            mm.short_name = get_string_key(h, "shortName");
            mm.level = get_long_key(h, "level", 0);
            mm.validity_date = get_long_key(h, "validityDate", 0);
            mm.validity_time = get_long_key(h, "validityTime", 0);
            mm.step = get_long_key(h, "step", 0);
            mm.npoints = nvals;
            if (mm.short_name.empty()) mm.short_name = "unknown";
            VarAgg &var_agg = var_aggs[mm.short_name];

            double ref_min = std::numeric_limits<double>::infinity();
            double ref_max = -std::numeric_limits<double>::infinity();
            double val_min = std::numeric_limits<double>::infinity();
            double val_max = -std::numeric_limits<double>::infinity();

            long double sum_abs = 0.0L, sum_sq = 0.0L, sum_bias = 0.0L;
            double msg_max_abs = 0.0;
            std::size_t msg_max_k = 0;
            for (size_t k = 0; k < nvals; ++k) {
                const double v = vals[k];
                const double r = static_cast<double>(ref[k]);
                ref_min = std::min(ref_min, r);
                ref_max = std::max(ref_max, r);
                val_min = std::min(val_min, v);
                val_max = std::max(val_max, v);
                const double e = v - r;
                const double ae = std::abs(e);
                sum_abs += ae;
                sum_sq += static_cast<long double>(e) * static_cast<long double>(e);
                sum_bias += e;

                g_ref_sum += r;
                g_ref_sq += static_cast<long double>(r) * static_cast<long double>(r);
                g_val_sum += v;
                g_val_sq += static_cast<long double>(v) * static_cast<long double>(v);

                var_agg.n += 1;
                var_agg.sum_sq_err += static_cast<long double>(e) * static_cast<long double>(e);
                var_agg.sum_ref += r;
                var_agg.sum_ref_sq += static_cast<long double>(r) * static_cast<long double>(r);
                var_agg.sum_val += v;
                var_agg.sum_val_sq += static_cast<long double>(v) * static_cast<long double>(v);
                var_agg.vmin = std::min(var_agg.vmin, std::min(r, v));
                var_agg.vmax = std::max(var_agg.vmax, std::max(r, v));

                if (ae > msg_max_abs) {
                    msg_max_abs = ae;
                    msg_max_k = k;
                }
            }

            const double range = std::max(1e-12, std::max(ref_max, val_max) - std::min(ref_min, val_min));
            mm.mae = static_cast<double>(sum_abs / static_cast<long double>(nvals));
            mm.rmse = std::sqrt(static_cast<double>(sum_sq / static_cast<long double>(nvals)));
            mm.nmae = mm.mae / range;
            mm.nrmse = mm.rmse / range;
            mm.max_abs = msg_max_abs;
            metrics.push_back(mm);

            g_abs += sum_abs;
            g_sq += sum_sq;
            g_bias += sum_bias;
            g_abs_norm += sum_abs / range;
            g_sq_norm += sum_sq / (range * range);
            g_n += static_cast<std::uint64_t>(nvals);

            if (msg_max_abs > global_max_abs) {
                global_max_abs = msg_max_abs;
                global_max_msg = msg_idx;
                global_max_point = msg_max_k;
                global_max_ref = static_cast<double>(ref[msg_max_k]);
                global_max_val = vals[msg_max_k];
            }

            codes_handle_delete(h);
            ++msg_idx;
        }

        std::fclose(fg);
        raw.close();

        if (g_n == 0 || metrics.empty()) {
            throw std::runtime_error("No comparable messages found");
        }

        const double mae = static_cast<double>(g_abs / static_cast<long double>(g_n));
        const double rmse = std::sqrt(static_cast<double>(g_sq / static_cast<long double>(g_n)));
        const double bias = static_cast<double>(g_bias / static_cast<long double>(g_n));
        const double nmae = static_cast<double>(g_abs_norm / static_cast<long double>(g_n));
        const double nrmse = std::sqrt(static_cast<double>(g_sq_norm / static_cast<long double>(g_n)));
        const double ref_rms = std::sqrt(std::max(
            0.0, static_cast<double>(g_ref_sq / static_cast<long double>(g_n))));
        const double rel_rmse = rmse / std::max(1e-12, ref_rms);

        const double mean_ref = static_cast<double>(g_ref_sum / static_cast<long double>(g_n));
        const double mean_val = static_cast<double>(g_val_sum / static_cast<long double>(g_n));
        const double var_ref = std::max(
            0.0, static_cast<double>(g_ref_sq / static_cast<long double>(g_n)) - mean_ref * mean_ref);
        const double var_val = std::max(
            0.0, static_cast<double>(g_val_sq / static_cast<long double>(g_n)) - mean_val * mean_val);
        const double var_ratio = var_val / std::max(1e-12, var_ref);
        const double var_rel_error = std::abs(var_val - var_ref) / std::max(1e-12, var_ref);

        std::vector<VarMetric> var_metrics;
        var_metrics.reserve(var_aggs.size());
        for (const auto &kv : var_aggs) {
            const auto &name = kv.first;
            const auto &a = kv.second;
            if (a.n == 0) continue;
            VarMetric vm;
            vm.short_name = name;
            vm.n = a.n;
            vm.rmse = std::sqrt(static_cast<double>(a.sum_sq_err / static_cast<long double>(a.n)));
            vm.value_range = std::max(1e-12, a.vmax - a.vmin);
            vm.nrmse = vm.rmse / vm.value_range;
            const double ref_rms_var = std::sqrt(std::max(
                0.0, static_cast<double>(a.sum_ref_sq / static_cast<long double>(a.n))));
            vm.rel_rmse = vm.rmse / std::max(1e-12, ref_rms_var);
            const double mref = static_cast<double>(a.sum_ref / static_cast<long double>(a.n));
            const double mval = static_cast<double>(a.sum_val / static_cast<long double>(a.n));
            vm.var_ref = std::max(
                0.0, static_cast<double>(a.sum_ref_sq / static_cast<long double>(a.n)) - mref * mref);
            vm.var_val = std::max(
                0.0, static_cast<double>(a.sum_val_sq / static_cast<long double>(a.n)) - mval * mval);
            vm.var_ratio = vm.var_val / std::max(1e-12, vm.var_ref);
            vm.var_rel_error = std::abs(vm.var_val - vm.var_ref) / std::max(1e-12, vm.var_ref);
            var_metrics.push_back(vm);
        }
        std::sort(var_metrics.begin(), var_metrics.end(), [](const VarMetric &a, const VarMetric &b) {
            return a.nrmse > b.nrmse;
        });

        double max_var_nrmse = 0.0;
        double min_var_nrmse = std::numeric_limits<double>::infinity();
        double max_var_rmse = 0.0;
        double max_var_rel_rmse = 0.0;
        for (const auto &vm : var_metrics) {
            max_var_nrmse = std::max(max_var_nrmse, vm.nrmse);
            min_var_nrmse = std::min(min_var_nrmse, vm.nrmse);
            max_var_rmse = std::max(max_var_rmse, vm.rmse);
            max_var_rel_rmse = std::max(max_var_rel_rmse, vm.rel_rmse);
        }
        if (!std::isfinite(min_var_nrmse)) min_var_nrmse = 0.0;
        const double range_var_nrmse = max_var_nrmse - min_var_nrmse;

        auto by_nmae = metrics;
        std::sort(by_nmae.begin(), by_nmae.end(), [](const MsgMetric &a, const MsgMetric &b) {
            return a.nmae > b.nmae;
        });
        auto by_max_abs = metrics;
        std::sort(by_max_abs.begin(), by_max_abs.end(), [](const MsgMetric &a, const MsgMetric &b) {
            return a.max_abs > b.max_abs;
        });

        std::ostringstream report;
        report << "ComputeError summary\n";
        report << "messages_compared=" << metrics.size() << "\n";
        report << "points_compared=" << g_n << "\n";
        report << "mae=" << fmt(mae) << "\n";
        report << "rmse=" << fmt(rmse) << "\n";
        report << "bias=" << fmt(bias) << "\n";
        report << "nmae_joint_range=" << fmt(nmae) << "\n";
        report << "nrmse_joint_range=" << fmt(nrmse) << "\n";
        report << "relative_rmse=" << fmt(rel_rmse) << "\n";
        report << "reference_variance=" << fmt(var_ref) << "\n";
        report << "decoded_variance=" << fmt(var_val) << "\n";
        report << "variance_ratio=" << fmt(var_ratio) << "\n";
        report << "variance_rel_error=" << fmt(var_rel_error) << "\n";
        report << "max_var_rmse=" << fmt(max_var_rmse) << "\n";
        report << "max_var_nrmse=" << fmt(max_var_nrmse) << "\n";
        report << "range_var_nrmse=" << fmt(range_var_nrmse) << "\n";
        report << "max_var_relative_rmse=" << fmt(max_var_rel_rmse) << "\n";
        report << "global_max_abs_error=" << fmt(global_max_abs) << "\n";
        report << "global_max_abs_message=" << global_max_msg << "\n";
        report << "global_max_abs_point_index=" << global_max_point << "\n";
        report << "global_max_abs_ref=" << fmt(global_max_ref) << "\n";
        report << "global_max_abs_grib=" << fmt(global_max_val) << "\n";

        report << "\nTop messages by normalized MAE\n";
        for (int i = 0; i < std::min<int>(opt.top_n, static_cast<int>(by_nmae.size())); ++i) {
            const auto &m = by_nmae[static_cast<size_t>(i)];
            report << "#" << (i + 1)
                   << " msg=" << m.msg_index
                   << " shortName=" << m.short_name
                   << " level=" << m.level
                   << " date=" << m.validity_date
                   << " time=" << m.validity_time
                   << " step=" << m.step
                   << " nmae=" << fmt(m.nmae)
                   << " rmse=" << fmt(m.rmse)
                   << " maxAbs=" << fmt(m.max_abs)
                   << "\n";
        }

        report << "\nTop messages by absolute max error\n";
        for (int i = 0; i < std::min<int>(opt.top_n, static_cast<int>(by_max_abs.size())); ++i) {
            const auto &m = by_max_abs[static_cast<size_t>(i)];
            report << "#" << (i + 1)
                   << " msg=" << m.msg_index
                   << " shortName=" << m.short_name
                   << " level=" << m.level
                   << " date=" << m.validity_date
                   << " time=" << m.validity_time
                   << " step=" << m.step
                   << " maxAbs=" << fmt(m.max_abs)
                   << " nmae=" << fmt(m.nmae)
                   << " rmse=" << fmt(m.rmse)
                   << "\n";
        }

        report << "\nPer-variable normalized RMSE / variance preservation\n";
        for (const auto &vm : var_metrics) {
            report << "var=" << vm.short_name
                   << " n=" << vm.n
                   << " rmse=" << fmt(vm.rmse)
                   << " nrmse=" << fmt(vm.nrmse)
                   << " rel_rmse=" << fmt(vm.rel_rmse)
                   << " range=" << fmt(vm.value_range)
                   << " var_ref=" << fmt(vm.var_ref)
                   << " var_decoded=" << fmt(vm.var_val)
                   << " var_ratio=" << fmt(vm.var_ratio)
                   << " var_rel_error=" << fmt(vm.var_rel_error)
                   << "\n";
        }

        std::cout << report.str();
        if (!opt.output_path.empty()) {
            std::ofstream out(opt.output_path, std::ios::out | std::ios::trunc);
            if (!out.is_open()) throw std::runtime_error("Cannot open output report: " + opt.output_path);
            out << report.str();
            out.close();
        }

        return 0;
    } catch (const std::exception &e) {
        std::cerr << "ComputeError error: " << e.what() << "\n";
        return 2;
    }
}
