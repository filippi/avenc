#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>
namespace avenc_compare {
inline uint64_t stable_seed(const std::string& s,uint64_t seed) {
    uint64_t h=14695981039346656037ULL;
    for(unsigned char c:s) { h^=c;h*=1099511628211ULL; }
    return h^seed;
}
// Explicit rejection sampling avoids implementation-specific uniform distributions.
inline uint64_t bounded(std::mt19937_64& rng,uint64_t bound) {
    uint64_t threshold=(-bound)%bound, x;
    do { x=rng(); } while(x<threshold);
    return x%bound;
}
inline std::vector<size_t> sample_indices(size_t n,size_t count,uint64_t seed) {
    std::vector<size_t> out;
    if(count>=n || count==0) { out.resize(n);for(size_t i=0;i<n;++i) out[i]=i;return out; }
    // Floyd's algorithm: O(sample size), without replacement.
    std::map<size_t,bool> selected;
    std::mt19937_64 rng(seed);
    for(size_t j=n-count;j<n;++j) {
        size_t k=static_cast<size_t>(bounded(rng,j+1));
        selected.emplace(selected.count(k)?j:k,true);
    }
    for(const auto& x:selected) out.push_back(x.first);
    return out;
}
// Spatially spread, deterministic draws: one point in every nearly equal
// contiguous scan-order block. No candidate values enter the selection.
inline std::vector<size_t> stratified_indices(size_t n,size_t count,uint64_t seed) {
    if(!count || count>=n) return sample_indices(n,0,seed);
    std::mt19937_64 rng(seed);
    std::vector<size_t> out;out.reserve(count);
    const size_t base=n/count,remainder=n%count;
    size_t begin=0;
    for(size_t i=0;i<count;++i) {
        size_t length=base+(i<remainder);
        out.push_back(begin+static_cast<size_t>(bounded(rng,length)));
        begin+=length;
    }
    return out;
}
inline double quantile(const std::vector<double>& sorted,double q) {
    if(sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
    double p=q*(sorted.size()-1);size_t i=static_cast<size_t>(p);
    return sorted[i]+(sorted[std::min(i+1,sorted.size()-1)]-sorted[i])*(p-i);
}
inline double ratio(double a,double b) {
    return b>0 ? a/b : std::numeric_limits<double>::quiet_NaN();
}
struct Accumulator {
    uint64_t n=0,examined=0,missing_reference=0,missing_decoded=0,missing_mismatch=0,both_missing=0;
    long double mean_x=0,mean_y=0,mean_e=0,m2_x=0,m2_y=0,m2_e=0,cov=0,abs_sum=0,sq_sum=0;
    double min_x=INFINITY,max_x=-INFINITY,min_y=INFINITY,max_y=-INFINITY,max_abs=0;
    size_t max_error_point=0;
    size_t cap;
    std::mt19937_64 rng;
    std::vector<std::array<double,3>> reservoir;
    explicit Accumulator(size_t capacity=200000,uint64_t seed=5489):cap(capacity),rng(seed) {}
    void add(double x,double y,size_t point) {
        ++examined;bool fx=std::isfinite(x),fy=std::isfinite(y);
        missing_reference+=!fx;missing_decoded+=!fy;missing_mismatch+=fx!=fy;both_missing+=!fx&&!fy;
        if(!fx||!fy) return;
        ++n;double e=y-x,ae=std::abs(e);
        long double dx=x-mean_x,dy=y-mean_y,de=e-mean_e;
        mean_x+=dx/n;mean_y+=dy/n;mean_e+=de/n;
        m2_x+=dx*(x-mean_x);m2_y+=dy*(y-mean_y);m2_e+=de*(e-mean_e);cov+=dx*(y-mean_y);
        abs_sum+=ae;sq_sum+=static_cast<long double>(e)*e;
        min_x=std::min(min_x,x);max_x=std::max(max_x,x);min_y=std::min(min_y,y);max_y=std::max(max_y,y);
        if(ae>max_abs) { max_abs=ae;max_error_point=point; }
        if(!cap || reservoir.size()<cap) reservoir.push_back({x,y,ae});
        else { uint64_t j=bounded(rng,n);if(j<cap) reservoir[j]={x,y,ae}; }
    }
    std::map<std::string,double> metrics() const {
        double nan=std::numeric_limits<double>::quiet_NaN();
        double rmse=n?std::sqrt(static_cast<double>(sq_sum/n)):nan;
        double sx=n?std::sqrt(std::max(0.0,static_cast<double>(m2_x/n))):nan;
        double sy=n?std::sqrt(std::max(0.0,static_cast<double>(m2_y/n))):nan;
        double rms=n?std::sqrt(static_cast<double>(m2_x/n+mean_x*mean_x)):nan;
        double range=n?max_x-min_x:nan;
        std::vector<double> x,y,e;x.reserve(reservoir.size());y.reserve(reservoir.size());e.reserve(reservoir.size());
        for(const auto& p:reservoir) { x.push_back(p[0]);y.push_back(p[1]);e.push_back(p[2]); }
        std::sort(x.begin(),x.end());std::sort(y.begin(),y.end());std::sort(e.begin(),e.end());
        auto db=[&](double signal) { return signal>0&&rmse>0?20*std::log10(signal/rmse):nan; };
        std::map<std::string,double> m{
            {"mae",n?static_cast<double>(abs_sum/n):nan},{"rmse",rmse},
            {"bias",n?static_cast<double>(mean_e):nan},
            {"error_std",n?std::sqrt(std::max(0.0,static_cast<double>(m2_e/n))):nan},
            {"median_abs_error",quantile(e,.5)},{"abs_error_p95",quantile(e,.95)},
            {"abs_error_p99",quantile(e,.99)},{"max_abs_error",n?max_abs:nan},
            {"reference_min",n?min_x:nan},{"reference_max",n?max_x:nan},
            {"decoded_min",n?min_y:nan},{"decoded_max",n?max_y:nan},
            {"min_shift",n?min_y-min_x:nan},{"max_shift",n?max_y-max_x:nan},
            {"reference_mean",n?static_cast<double>(mean_x):nan},
            {"decoded_mean",n?static_cast<double>(mean_y):nan},
            {"reference_std",sx},{"decoded_std",sy},{"reference_rms",rms},
            {"std_ratio",ratio(sy,sx)},{"variance_ratio",ratio(sy*sy,sx*sx)},
            {"correlation",m2_x>0&&m2_y>0?std::clamp(static_cast<double>(cov/std::sqrt(m2_x*m2_y)),-1.0,1.0):nan},
            {"relative_rmse",ratio(rmse,rms)},{"nrmse_range",ratio(rmse,range)},
            {"nrmse_q01_q99",ratio(rmse,quantile(x,.99)-quantile(x,.01))},
            {"snr_rms_db",db(rms)},{"snr_std_db",db(sx)},{"psnr_db",db(range)}
        };
        const double qs[]={.01,.05,.5,.95,.99};const char* names[]={"q01","q05","q50","q95","q99"};
        for(size_t i=0;i<5;++i) {
            std::string name=names[i];double a=quantile(x,qs[i]),b=quantile(y,qs[i]);
            m[name+"_reference"]=a;m[name+"_decoded"]=b;m[name+"_shift"]=b-a;
        }
        return m;
    }
};
} // namespace avenc_compare
