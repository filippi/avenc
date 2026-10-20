#include "avenc_version.h"
#include <eccodes.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "embedded_cep_template.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

struct TestDef {
    int id = 0;
    std::string slug;
    std::string short_name;
    long param_id = 0;
    std::string description;
};

struct Options {
    std::string template_grib;  // optional override
    std::string out_dir = "synthetic_tests";
    std::string combined_out;  // empty means <out_dir>/synthetic_all.grib
    std::string raw_out;       // empty means disabled
    int ni = -1;               // default: template Ni
    int nj = -1;               // default: template Nj
    int nlev = 10;
    int nt = 20;
    int seed = 42;
    bool separate = false;
    bool small = false;
};

struct TemplateInfo {
    std::vector<unsigned char> message;
    long ni = 0;
    long nj = 0;
    long data_date = 20250101;
    long data_time = 0;
};

struct SpectralMode {
    double kx = 0.0;
    double ky = 0.0;
    double amp = 0.0;
    double omega = 0.0;
    double phi = 0.0;
};

[[noreturn]] void usage(const char *prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  --template <file.grib> Optional template override. Default: embedded tiny template (resources/tiny.grib)\n"
        << "  --out-dir <dir>        Output directory (default: synthetic_tests)\n"
        << "  --combined <file.grib> Combined output file (default: <out-dir>/synthetic_all.grib)\n"
        << "  --raw <file.bin>       Write concatenated raw float32 + CSV index <file.bin>.idx\n"
        << "  --ni <int>             Grid width override (default: template Ni)\n"
        << "  --nj <int>             Grid height override (default: template Nj)\n"
        << "  --nlev <int>           Number of levels per test (default: 10)\n"
        << "  --nt <int>             Number of time steps per test (default: 20)\n"
        << "  --small                Compact mode defaults: --nlev 3 --nt 2 (unless explicitly overridden)\n"
        << "  --seed <int>           Random seed (default: 42)\n"
        << "  --separate             Also write per-test GRIB files\n";
    std::exit(1);
}

double wrap_periodic(double a) {
    if (a < -0.5) return a + 1.0;
    if (a > 0.5) return a - 1.0;
    return a;
}

bool set_long_if_possible(codes_handle *h, const char *key, long value) {
    return codes_set_long(h, key, value) == CODES_SUCCESS;
}

bool set_string_if_possible(codes_handle *h, const char *key, const std::string &value) {
    return codes_set_string(h, key, value.c_str(), nullptr) == CODES_SUCCESS;
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

Options parse_args(int argc, char **argv) {
    Options opt;
    bool nlev_set = false;
    bool nt_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need_val = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                usage(argv[0]);
            }
            return argv[++i];
        };
        if (a == "--template") {
            opt.template_grib = need_val("--template");
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
        } else if (a == "--out-dir") {
            opt.out_dir = need_val("--out-dir");
        } else if (a == "--combined") {
            opt.combined_out = need_val("--combined");
        } else if (a == "--raw") {
            opt.raw_out = need_val("--raw");
        } else if (a == "--ni") {
            opt.ni = std::stoi(need_val("--ni"));
        } else if (a == "--nj") {
            opt.nj = std::stoi(need_val("--nj"));
        } else if (a == "--nlev") {
            opt.nlev = std::stoi(need_val("--nlev"));
            nlev_set = true;
        } else if (a == "--nt") {
            opt.nt = std::stoi(need_val("--nt"));
            nt_set = true;
        } else if (a == "--small") {
            opt.small = true;
        } else if (a == "--seed") {
            opt.seed = std::stoi(need_val("--seed"));
        } else if (a == "--separate") {
            opt.separate = true;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "Unknown option: " << a << "\n";
            usage(argv[0]);
        } else {
            std::cerr << "Unexpected positional argument: " << a << "\n";
            usage(argv[0]);
        }
    }
    if (opt.small) {
        if (!nlev_set) opt.nlev = 3;
        if (!nt_set) opt.nt = 2;
    }
    if (opt.nlev <= 0 || opt.nt <= 0) {
        std::cerr << "--nlev and --nt must be positive\n";
        usage(argv[0]);
    }
    return opt;
}

TemplateInfo load_template(const std::string &path) {
    TemplateInfo t;
    codes_handle *h = nullptr;
    if (path.empty()) {
        h = codes_handle_new_from_message_copy(nullptr, kEmbeddedCepTemplateGrib, kEmbeddedCepTemplateGribLen);
        if (!h) throw std::runtime_error("Failed to build handle from embedded template");
    } else {
        FILE *fp = std::fopen(path.c_str(), "rb");
        if (!fp) throw std::runtime_error("Cannot open template GRIB: " + path);
        int err = 0;
        h = codes_handle_new_from_file(nullptr, fp, PRODUCT_GRIB, &err);
        std::fclose(fp);
        if (!h) throw std::runtime_error("Failed to read first message from template GRIB");
    }

    const void *msg = nullptr;
    size_t msg_size = 0;
    if (codes_get_message(h, &msg, &msg_size) != CODES_SUCCESS || !msg || msg_size == 0) {
        codes_handle_delete(h);
        throw std::runtime_error("Failed to serialize template message");
    }
    t.message.resize(msg_size);
    std::memcpy(t.message.data(), msg, msg_size);

    (void)codes_get_long(h, "Ni", &t.ni);
    (void)codes_get_long(h, "Nj", &t.nj);
    (void)codes_get_long(h, "dataDate", &t.data_date);
    (void)codes_get_long(h, "dataTime", &t.data_time);

    codes_handle_delete(h);
    if (t.ni <= 0 || t.nj <= 0) {
        throw std::runtime_error("Template does not expose valid Ni/Nj");
    }
    return t;
}

std::vector<SpectralMode> build_modes(int seed, int nmode = 12) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::vector<SpectralMode> out;
    out.reserve(static_cast<size_t>(nmode));
    for (int i = 0; i < nmode; ++i) {
        const double theta = 2.0 * kPi * u01(rng);
        const double k = 2.0 + 120.0 * std::pow(u01(rng), 1.2);
        SpectralMode m;
        m.kx = k * std::cos(theta);
        m.ky = k * std::sin(theta);
        m.amp = std::pow(std::max(1.0, k), -5.0 / 6.0);
        m.omega = 0.5 + 1.5 * u01(rng);
        m.phi = 2.0 * kPi * u01(rng);
        out.push_back(m);
    }
    return out;
}

void generate_test_field(int test_id,
                         int lev,
                         int t,
                         int nlev,
                         int nt,
                         int ni,
                         int nj,
                         const std::vector<double> &x,
                         const std::vector<double> &y,
                         const std::vector<SpectralMode> &modes,
                         std::vector<double> &out) {
    const double lfrac = (nlev <= 1) ? 0.0 : static_cast<double>(lev) / static_cast<double>(nlev - 1);
    const double tf = (nt <= 1) ? 0.0 : static_cast<double>(t) / static_cast<double>(nt - 1);

    for (int j = 0; j < nj; ++j) {
        const double yy = y[static_cast<size_t>(j)];
        for (int i = 0; i < ni; ++i) {
            const double xx = x[static_cast<size_t>(i)];
            double v = 0.0;

            if (test_id == 1) {
                // Test 1: translated Gaussian blob.
                const double cx = std::fmod(0.1 + 0.8 * tf, 1.0);
                const double cy = 0.5 + 0.15 * std::sin(2.0 * kPi * lfrac);
                const double sx = wrap_periodic(xx - cx);
                const double sy = wrap_periodic(yy - cy);
                const double sigma = 0.04 + 0.02 * lfrac;
                const double g = std::exp(-(sx * sx + sy * sy) / (2.0 * sigma * sigma));
                v = 280.0 + 6.0 * lfrac + 20.0 * g;
            } else if (test_id == 2) {
                // Test 2: rotating sharp cross-like pattern.
                const double ang = 2.0 * kPi * tf + 0.8 * lfrac;
                const double c = std::cos(ang), s = std::sin(ang);
                const double xc = xx - 0.5, yc = yy - 0.5;
                const double xr = c * xc - s * yc;
                const double yr = s * xc + c * yc;
                const double w = 0.03;
                const double eps = 0.0035;
                const double hx = 0.5 * (std::tanh((w - std::abs(xr)) / eps) + 1.0);
                const double hy = 0.5 * (std::tanh((w - std::abs(yr)) / eps) + 1.0);
                const double shape = std::min(1.0, hx + hy);
                v = 5.0 * shape + 0.2 * std::sin(12.0 * kPi * xx) * std::sin(12.0 * kPi * yy);
            } else if (test_id == 4) {
                // Test 4: multi-sine superposition.
                v = 0.0;
                v += 1.6 * std::sin(2.0 * kPi * (2.0 * xx + 1.0 * yy + 1.5 * tf + 0.1 * lfrac));
                v += 1.2 * std::sin(2.0 * kPi * (6.0 * xx + 3.0 * yy + 0.7 * tf));
                v += 0.8 * std::cos(2.0 * kPi * (11.0 * xx - 5.0 * yy + 0.9 * tf + 0.2 * lfrac));
                v += 0.5 * std::sin(2.0 * kPi * (19.0 * xx + 13.0 * yy + 0.4 * tf));
            } else if (test_id == 6) {
                // Test 6: pseudo turbulence-like field (k^{-5/3}-style modal amplitudes).
                v = 0.0;
                for (const auto &m : modes) {
                    const double ph = 2.0 * kPi * (m.kx * xx + m.ky * yy + m.omega * tf) + m.phi + 0.12 * lfrac;
                    v += m.amp * std::sin(ph);
                }
                v *= 8.0;
            } else if (test_id == 7) {
                // Test 7: sparse intermittent rain-like field.
                const double c1x = std::fmod(0.2 + 0.6 * tf, 1.0);
                const double c1y = 0.3 + 0.12 * std::sin(2.0 * kPi * tf);
                const double c2x = std::fmod(0.8 - 0.4 * tf + 1.0, 1.0);
                const double c2y = 0.7 + 0.10 * std::cos(2.0 * kPi * tf);
                const double dx1 = wrap_periodic(xx - c1x), dy1 = wrap_periodic(yy - c1y);
                const double dx2 = wrap_periodic(xx - c2x), dy2 = wrap_periodic(yy - c2y);
                const double s1 = std::exp(-(dx1 * dx1 + dy1 * dy1) / (2.0 * 0.05 * 0.05));
                const double s2 = 0.8 * std::exp(-(dx2 * dx2 + dy2 * dy2) / (2.0 * 0.08 * 0.08));
                double rain = std::max(0.0, s1 + s2 - 0.55);
                rain = std::pow(rain, 2.7) * 90.0;
                const double micro = std::sin(2.0 * kPi * (37.0 * xx + 53.0 * yy + 0.13 * tf));
                if (micro > 0.995) rain += 2.0 + 5.0 * (micro - 0.995) / 0.005;
                v = rain * std::exp(-2.0 * lfrac);
            } else if (test_id == 8) {
                // Test 8: level-structured 3D profile with horizontal perturbations.
                const double profile = 1000.0 - 12.0 * static_cast<double>(lev) + 20.0 * std::sin(2.0 * kPi * tf);
                const double h = std::sin(2.0 * kPi * (3.0 * xx + 2.0 * yy + tf)) +
                                 0.6 * std::cos(2.0 * kPi * (7.0 * xx - 4.0 * yy + 0.3 * tf));
                v = profile + (30.0 + 10.0 * lfrac) * h;
            } else {
                v = 0.0;
            }

            out[static_cast<size_t>(j) * static_cast<size_t>(ni) + static_cast<size_t>(i)] = v;
        }
    }
}

void write_message(FILE *f, codes_handle *h) {
    const void *msg = nullptr;
    size_t sz = 0;
    if (codes_get_message(h, &msg, &sz) != CODES_SUCCESS || !msg || sz == 0) {
        throw std::runtime_error("Failed to serialize GRIB message");
    }
    if (std::fwrite(msg, 1, sz, f) != sz) {
        throw std::runtime_error("Failed to write GRIB message");
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (avenc_print_version(argc, argv)) return 0;
    try {
        const Options opt = parse_args(argc, argv);
        std::filesystem::create_directories(opt.out_dir);

        const TemplateInfo tmpl = load_template(opt.template_grib);
        const int ni = (opt.ni > 0) ? opt.ni : static_cast<int>(tmpl.ni);
        const int nj = (opt.nj > 0) ? opt.nj : static_cast<int>(tmpl.nj);
        if (ni <= 0 || nj <= 0) throw std::runtime_error("Invalid Ni/Nj");
        const size_t npts = static_cast<size_t>(ni) * static_cast<size_t>(nj);

        std::vector<TestDef> tests = {
            {1, "test01_gaussian_advection", "t", 130, "Translated Gaussian blob"},
            {2, "test02_rotating_sharp_shape", "10u", 165, "Rotating sharp cross-like shape"},
            {4, "test04_multisine", "q", 133, "Multi-sine frequency mix"},
            {6, "test06_kolmogorov_like", "z", 129, "Random-phase k^(-5/3)-style modal field"},
            {7, "test07_sparse_rain", "tp", 228, "Sparse intermittent rain-like field"},
            {8, "test08_level_structured", "r", 157, "Level-structured profile field"},
        };

        const std::string combined_path =
            opt.combined_out.empty() ? (std::filesystem::path(opt.out_dir) / "synthetic_all.grib").string() : opt.combined_out;
        FILE *f_combined = std::fopen(combined_path.c_str(), "wb");
        if (!f_combined) throw std::runtime_error("Cannot open combined output: " + combined_path);

        std::ofstream raw;
        std::ofstream raw_idx;
        if (!opt.raw_out.empty()) {
            raw.open(opt.raw_out, std::ios::binary | std::ios::trunc);
            raw_idx.open(opt.raw_out + ".idx", std::ios::out | std::ios::trunc);
            if (!raw.is_open() || !raw_idx.is_open()) {
                std::fclose(f_combined);
                throw std::runtime_error("Cannot open raw outputs");
            }
            raw_idx << "messageIndex,testId,testSlug,shortName,levelIndex,timeIndex,ni,nj,numPoints,byteOffset,gribFile\n";
        }

        std::vector<double> x(static_cast<size_t>(ni));
        std::vector<double> y(static_cast<size_t>(nj));
        for (int i = 0; i < ni; ++i) x[static_cast<size_t>(i)] = (static_cast<double>(i) + 0.5) / static_cast<double>(ni);
        for (int j = 0; j < nj; ++j) y[static_cast<size_t>(j)] = (static_cast<double>(j) + 0.5) / static_cast<double>(nj);

        const auto modes = build_modes(opt.seed + 17, 12);
        std::vector<double> values(npts, 0.0);
        std::vector<float> f32(npts, 0.0f);
        std::uint64_t byte_offset = 0;
        std::uint64_t msg_index = 0;

        for (const auto &test : tests) {
            FILE *f_sep = nullptr;
            std::string sep_name = (std::filesystem::path(opt.out_dir) / (test.slug + ".grib")).string();
            if (opt.separate) {
                f_sep = std::fopen(sep_name.c_str(), "wb");
                if (!f_sep) {
                    std::fclose(f_combined);
                    throw std::runtime_error("Cannot open per-test output: " + sep_name);
                }
            }

            for (int lev = 0; lev < opt.nlev; ++lev) {
                for (int t = 0; t < opt.nt; ++t) {
                    codes_handle *h = codes_handle_new_from_message_copy(nullptr, tmpl.message.data(), tmpl.message.size());
                    if (!h) {
                        if (f_sep) std::fclose(f_sep);
                        std::fclose(f_combined);
                        throw std::runtime_error("Failed to create GRIB handle from template");
                    }

                    if (ni != tmpl.ni || nj != tmpl.nj) {
                        (void)set_long_if_possible(h, "Ni", ni);
                        (void)set_long_if_possible(h, "Nj", nj);
                        (void)set_long_if_possible(h, "numberOfDataPoints", static_cast<long>(npts));
                    }
                    (void)set_string_if_possible(h, "typeOfLevel", "isobaricInhPa");
                    (void)set_long_if_possible(h, "level", static_cast<long>(lev + 1));
                    (void)set_long_if_possible(h, "step", static_cast<long>(t));
                    (void)set_long_if_possible(h, "stepRange", static_cast<long>(t));
                    (void)set_long_if_possible(h, "forecastTime", static_cast<long>(t));
                    (void)set_long_if_possible(h, "dataDate", tmpl.data_date);
                    (void)set_long_if_possible(h, "dataTime", tmpl.data_time);
                    if (!set_string_if_possible(h, "shortName", test.short_name)) {
                        (void)set_long_if_possible(h, "paramId", test.param_id);
                    }

                    generate_test_field(test.id, lev, t, opt.nlev, opt.nt, ni, nj, x, y, modes, values);
                    if (codes_set_double_array(h, "values", values.data(), npts) != CODES_SUCCESS) {
                        codes_handle_delete(h);
                        if (f_sep) std::fclose(f_sep);
                        std::fclose(f_combined);
                        throw std::runtime_error("Failed to set values on GRIB message");
                    }

                    write_message(f_combined, h);
                    if (f_sep) write_message(f_sep, h);

                    if (!opt.raw_out.empty()) {
                        for (size_t i = 0; i < npts; ++i) f32[i] = static_cast<float>(values[i]);
                        raw.write(reinterpret_cast<const char *>(f32.data()), static_cast<std::streamsize>(npts * sizeof(float)));
                        if (!raw.good()) {
                            codes_handle_delete(h);
                            if (f_sep) std::fclose(f_sep);
                            std::fclose(f_combined);
                            throw std::runtime_error("Failed writing raw data");
                        }
                        raw_idx << msg_index << ","
                                << test.id << ","
                                << csv_escape(test.slug) << ","
                                << csv_escape(test.short_name) << ","
                                << lev << ","
                                << t << ","
                                << ni << ","
                                << nj << ","
                                << npts << ","
                                << byte_offset << ","
                                << csv_escape(test.slug + ".grib")
                                << "\n";
                        byte_offset += static_cast<std::uint64_t>(npts * sizeof(float));
                    }

                    codes_handle_delete(h);
                    ++msg_index;
                    if ((msg_index % 200) == 0) {
                        std::cerr << "\rGenerated messages: " << msg_index << std::flush;
                    }
                }
            }
            if (f_sep) std::fclose(f_sep);
        }

        std::fclose(f_combined);
        if (!opt.raw_out.empty()) {
            raw.close();
            raw_idx.close();
        }

        std::cerr << "\rGenerated messages: " << msg_index << "\n";
        if (opt.template_grib.empty()) {
            std::cout << "Template source: embedded tiny template (resources/tiny.grib)\n";
        } else {
            std::cout << "Template source: " << opt.template_grib << "\n";
        }
        std::cout << "Wrote combined GRIB: " << combined_path << "\n";
        if (opt.separate) {
            std::cout << "Wrote per-test GRIB files in: " << opt.out_dir << "\n";
        }
        if (!opt.raw_out.empty()) {
            std::cout << "Wrote raw float32: " << opt.raw_out << "\n";
            std::cout << "Wrote raw index: " << opt.raw_out << ".idx\n";
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "avenc_test_generator error: " << e.what() << "\n";
        return 2;
    }
}
