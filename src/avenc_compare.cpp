#include "avenc_version.h"
#include "avenc_grib_time.h"
// Scientific identity-matched comparison. Legacy ComputeError remains available.
#include <eccodes.h>
#include "avenc_subset_decode.h"
#include "avenc_compare_metadata.h"
#include "avenc_compare_metrics.h"
#include "avenc_compare_sha256.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <set>
#include <tuple>
#include <sys/resource.h>
#include <unistd.h>
namespace fs=std::filesystem;
using namespace avenc_compare;
namespace {
using Clock=std::chrono::steady_clock;
using Handle=std::unique_ptr<codes_handle,decltype(&codes_handle_delete)>;
struct FileClose { void operator()(FILE* p) const { if(p) fclose(p); } };
using File=std::unique_ptr<FILE,FileClose>;
struct Options {
    std::string reference,candidate,json,csv,messages_csv;
    bool full=false,fast=false,hash=true,allow_unmatched=false,group_member=false,first_duplicate=false;
    size_t frames=8,points=10000,quantile_cap=200000,batch=8,top=5;
    uint64_t seed=5489;
};
void help() {
    std::cout<<
        "Usage: avenc-compare [options] ORIGINAL.grib|DIRECTORY CANDIDATE.mp4|.grib|DIRECTORY\n"
        "Directories: immediate .grib/.grb/.grib2/.grb2 files, sorted by path.\n"
        "  --fast                   Sampled: 7 time-stratified fields/group, 8192 points/field; no hashes\n"
        "  --full                   Every matched field and grid point (exclusive with --fast)\n"
        "  --frames-per-group N     Fast mode fields per scientific group (default 8)\n"
        "  --points-per-frame N     Fast mode grid points per field (default 10000)\n"
        "  --seed N                 Deterministic sampling seed (default 5489)\n"
        "  --group-by-member        Separate ensemble members in group summaries\n"
        "  --quantile-cap N         Reservoir per group AND field (default 200000; 0 exact/unbounded)\n"
        "  --decode-batch N         Maximum consecutive MP4 frames per read (default 8)\n"
        "  --allow-unmatched        Compare intersection; report missing/extra identities\n"
        "  --duplicate-policy first Explicitly retain first occurrence; default rejects duplicates\n"
        "  --hash                   Include SHA-256, including in --fast mode\n"
        "  --no-hash                Skip source SHA-256 (sizes/mtime still recorded)\n"
        "  --json PATH              Provenance, group and field metrics, worst fields\n"
        "  --csv PATH               Group metrics CSV\n"
        "  --messages-csv PATH      Selected field metrics CSV\n"
        "  --top N                  Worst fields per criterion (default 5)\n"
        "  -h, --help               Show help\n"
        "No GRIB reconstruction or full dataset value cube is created.\n";
}
size_t integer(const std::string& s,bool zero=false) {
    if(s.empty() || s[0]=='-') throw std::runtime_error("Expected non-negative integer: "+s);
    size_t end=0;auto n=std::stoull(s,&end);
    if(end!=s.size() || (!zero && n==0) || n>std::numeric_limits<size_t>::max())
        throw std::runtime_error("Invalid integer: "+s);
    return static_cast<size_t>(n);
}
Options parse(int argc,char** argv) {
    Options o;std::vector<std::string> pos;
    bool frames_set=false,points_set=false,cap_set=false,hash_set=false;
    for(int i=1;i<argc;++i) {
        std::string a=argv[i];
        auto value=[&]() { if(++i>=argc) throw std::runtime_error("Missing value for "+a);return std::string(argv[i]); };
        if(a=="--full") o.full=true;
        else if(a=="--fast") o.fast=true;
        else if(a=="--hash") { o.hash=true;hash_set=true; }
        else if(a=="--no-hash") { o.hash=false;hash_set=true; }
        else if(a=="--allow-unmatched") o.allow_unmatched=true;
        else if(a=="--group-by-member") o.group_member=true;
        else if(a=="--frames-per-group") { o.frames=integer(value());frames_set=true; }
        else if(a=="--points-per-frame") { o.points=integer(value());points_set=true; }
        else if(a=="--quantile-cap") { o.quantile_cap=integer(value(),true);cap_set=true; }
        else if(a=="--decode-batch") o.batch=integer(value());
        else if(a=="--seed") o.seed=integer(value(),true);
        else if(a=="--top") o.top=integer(value());
        else if(a=="--json") o.json=value();
        else if(a=="--csv") o.csv=value();
        else if(a=="--messages-csv") o.messages_csv=value();
        else if(a=="--duplicate-policy") {
            if(value()!="first") throw std::runtime_error("Only explicit duplicate policy 'first' is supported");
            o.first_duplicate=true;
        }
        else if(a=="--help" || a=="-h") { help();std::exit(0); }
        else if(!a.empty()&&a[0]=='-') throw std::runtime_error("Unknown option: "+a);
        else pos.push_back(a);
    }
    if(o.fast && o.full) throw std::runtime_error("--fast and --full are mutually exclusive");
    if(o.fast) {
        if(!frames_set) o.frames=7;
        if(!points_set) o.points=8192;
        if(!cap_set) o.quantile_cap=16384;
        if(!hash_set) o.hash=false;
    }
    if(pos.size()!=2) throw std::runtime_error("Expected original and candidate inputs; use --help");
    if(o.batch>static_cast<size_t>(INT32_MAX)) throw std::runtime_error("decode-batch exceeds decoder limits");
    o.reference=pos[0];o.candidate=pos[1];return o;
}
std::string text(codes_handle* h,const char* key) {
    int err=0;if(codes_is_missing(h,key,&err) || err) return "";
    size_t n=0;if(codes_get_length(h,key,&n)) return "";
    std::vector<char> b(n+2);n=b.size();
    if(codes_get_string(h,key,b.data(),&n)) return "";
    return std::string(b.data());
}
size_t point_count(codes_handle* h) {
    size_t n=0;if(codes_get_size(h,"values",&n) || !n) throw std::runtime_error("Invalid GRIB values size");
    return n;
}
void set_text(codes_handle* h,const char* key,const std::string& value) {
    if(value.empty() || value=="-") return;
    int type=0, err=0;
    if(codes_get_native_type(h,key,&type)) throw std::runtime_error(std::string("Missing template key: ")+key);
    if(type==CODES_TYPE_LONG) err=codes_set_long(h,key,std::stol(value));
    else if(type==CODES_TYPE_DOUBLE) err=codes_set_double(h,key,std::stod(value));
    else { size_t n=value.size();err=codes_set_string(h,key,value.c_str(),&n); }
    if(err) throw std::runtime_error(std::string("Cannot restore metadata key ")+key+"="+value);
}
std::string keys(codes_handle* h,std::initializer_list<const char*> names) {
    std::string out;
    for(auto k:names) { auto v=text(h,k);out+=std::string(k)+"="+std::to_string(v.size())+":"+v+"|"; }
    return out;
}
struct Field {
    std::string identity,group,short_name,units,path;
    uint64_t offset=0;
    size_t message=0,npoints=0;
    int stream=-1;
    size_t frame=0;
    std::map<std::string,std::string> coordinates;
};
Field describe(codes_handle* h,const Options& o) {
    Field f;f.npoints=point_count(h);f.short_name=text(h,"shortName");f.units=text(h,"units");
    if(text(h,"md5GridSection").empty()) throw std::runtime_error("Cannot establish grid identity (md5GridSection absent)");
    f.group=keys(h,{"paramId","shortName","units","typeOfLevel","level","topLevel","bottomLevel","stepType","md5GridSection"});
    // Names/paramId can be unresolved with older ecCodes definitions. Raw
    // GRIB parameter codes remain authoritative and must not collapse together.
    if(f.short_name=="unknown" || text(h,"paramId").empty() || text(h,"paramId")=="0")
        f.group+=keys(h,{"edition","discipline","parameterCategory","parameterNumber",
                         "table2Version","indicatorOfParameter","centre","subCentre",
                         "tablesVersion","localTablesVersion"});
    if(o.group_member) f.group+=keys(h,{"number","perturbationNumber"});
    // Statistical fields can share an interval while having different valid times
    // (for example, an initialization placeholder and the first forecast field).
    f.identity=f.group+keys(h,{"dataDate","dataTime","validityDate","validityTime","startStep","endStep","stepUnits",
                              "number","perturbationNumber","typeOfEnsembleForecast","percentileValue",
                              "probabilityType","scaledValueOfLowerLimit","scaledValueOfUpperLimit"});
    f.identity+="npoints="+std::to_string(f.npoints);
    for(const auto* key:{"edition","discipline","parameterCategory","parameterNumber","table2Version","indicatorOfParameter","centre","subCentre","tablesVersion","localTablesVersion","paramId","typeOfLevel","level","topLevel","bottomLevel","stepType","md5GridSection",
                         "dataDate","dataTime","validityDate","validityTime","startStep","endStep","stepUnits","number","perturbationNumber"})
        f.coordinates[key]=text(h,key);
    return f;
}
// Select one reference field per chronological stratum. Member diversity is
// preferred within each sampled time, without inspecting candidate values.
std::vector<size_t> fast_fields(const std::vector<std::pair<size_t,size_t>>& pairs,
                                const std::vector<Field>& refs,size_t count,uint64_t seed) {
    const size_t wanted=std::min(count,pairs.size());
    if(wanted==pairs.size()) return sample_indices(pairs.size(),0,seed);
    using Time=std::tuple<long,long,double,double>;
    std::map<Time,std::vector<size_t>> by_time;
    auto numeric=[](const Field& f,const std::string& key) {
        auto it=f.coordinates.find(key);
        return it==f.coordinates.end()||it->second.empty()?0.0:std::stod(it->second);
    };
    auto member=[](const Field& f) {
        auto get=[&](const char* key) {
            auto it=f.coordinates.find(key);return it==f.coordinates.end()?std::string():it->second;
        };
        return get("number")+":"+get("perturbationNumber");
    };
    for(size_t i=0;i<pairs.size();++i) {
        const auto& f=refs[pairs[i].first];
        by_time[{static_cast<long>(numeric(f,"validityDate")),static_cast<long>(numeric(f,"validityTime")),
                 numeric(f,"startStep"),numeric(f,"endStep")}].push_back(i);
    }
    std::vector<const std::vector<size_t>*> times;
    for(const auto& t:by_time) times.push_back(&t.second);
    std::mt19937_64 rng(seed);
    std::set<std::string> members;
    std::set<size_t> selected;
    const size_t strata=std::min(wanted,times.size());
    for(size_t i=0;i<strata;++i) {
        size_t begin=i*times.size()/strata,end=(i+1)*times.size()/strata;
        const auto& options=*times[begin+bounded(rng,end-begin)];
        std::vector<size_t> fresh;
        for(auto j:options) if(!members.count(member(refs[pairs[j].first]))) fresh.push_back(j);
        const auto& pool=fresh.empty()?options:fresh;
        size_t j=pool[bounded(rng,pool.size())];
        selected.insert(j);members.insert(member(refs[pairs[j].first]));
    }
    // Custom budgets can exceed the number of distinct times.
    if(selected.size()<wanted) {
        std::vector<size_t> rest;
        for(size_t i=0;i<pairs.size();++i) if(!selected.count(i)) rest.push_back(i);
        for(auto j:sample_indices(rest.size(),wanted-selected.size(),rng())) selected.insert(rest[j]);
    }
    return {selected.begin(),selected.end()};
}
struct Source {
    std::string path,sha;
    uint64_t bytes;
    fs::file_time_type mtime;
};
Source source(const std::string& path,bool hash) {
    Source s{fs::absolute(path).lexically_normal().string(),"",fs::file_size(path),fs::last_write_time(path)};
    if(hash) s.sha=sha256_file(s.path);
    if(s.bytes!=fs::file_size(path) || s.mtime!=fs::last_write_time(path)) throw std::runtime_error("Input changed while hashing: "+path);
    return s;
}
std::vector<std::string> grib_paths(const std::string& input) {
    std::vector<std::string> out;
    if(fs::is_directory(input)) {
        for(const auto& e:fs::directory_iterator(input)) {
            auto ext=e.path().extension().string();
            if(e.is_regular_file() && (ext==".grib"||ext==".grb"||ext==".grib2"||ext==".grb2")) out.push_back(e.path().string());
        }
    } else if(fs::is_regular_file(input)) out.push_back(input);
    if(out.empty()) throw std::runtime_error("No GRIB files found: "+input);
    std::sort(out.begin(),out.end());return out;
}
std::vector<Field> index_gribs(const std::string& input,const Options& o,std::vector<Source>& sources) {
    std::vector<Field> fields;
    for(const auto& path:grib_paths(input)) {
        sources.push_back(source(path,o.hash));
        File fp(fopen(path.c_str(),"rb"));if(!fp) throw std::runtime_error("Cannot open GRIB: "+path);
        size_t message=0;
        while(true) {
            auto offset=ftello(fp.get());int err=0;
            Handle h(codes_handle_new_from_file(nullptr,fp.get(),PRODUCT_GRIB,&err),codes_handle_delete);
            if(!h) { if(err && err!=CODES_END_OF_FILE) throw std::runtime_error("GRIB scan failed: "+path);break; }
            Field f=describe(h.get(),o);f.path=sources.back().path;f.offset=static_cast<uint64_t>(offset);f.message=message++;
            fields.push_back(std::move(f));
        }
    }
    return fields;
}
std::vector<Field> index_mp4(const std::string& path,const Options& o,std::vector<Source>& sources) {
    namespace meta=avenc_compare_metadata;
    sources.push_back(source(path,o.hash));auto recon=meta::read(path);
    std::map<int,const meta::StreamInfo*> streams;
    for(const auto& s:recon.streams)
        if(!streams.emplace(s.stream_index,&s).second) throw std::runtime_error("Duplicate MP4 stream index");
    std::vector<Field> fields;std::set<std::pair<int,size_t>> frames;
    for(const auto& r:recon.records) {
        auto it=streams.find(r.stream_index);
        if(it==streams.end() || r.frame_index>=it->second->frame_count || r.frame_index>INT32_MAX)
            throw std::runtime_error("Invalid metadata stream/frame mapping");
        if(!frames.emplace(r.stream_index,r.frame_index).second) throw std::runtime_error("Repeated metadata stream/frame mapping");
        const auto& s=*it->second;
        auto bytes=meta::base64_decode(r.template_b64.empty()||r.template_b64=="-"?s.template_b64:r.template_b64);
        Handle h(codes_handle_new_from_message_copy(nullptr,bytes.data(),bytes.size()),codes_handle_delete);
        if(!h) throw std::runtime_error("Invalid embedded GRIB template");
        set_text(h.get(),"dataDate",r.data_date);set_text(h.get(),"dataTime",r.data_time);
        set_text(h.get(),"stepRange",r.step_range);set_text(h.get(),"number",r.ensemble_number);
        set_text(h.get(),"perturbationNumber",r.perturbation_number);
        avenc_grib_time::restore(h.get(),r.timestamp);
        Field f=describe(h.get(),o);
        if(f.short_name!=r.short_name || text(h.get(),"typeOfLevel")!=r.type_of_level || text(h.get(),"level")!=r.level)
            throw std::runtime_error("Embedded template and record variable/level disagree");
        if(static_cast<uint64_t>(s.crop_nx)*s.crop_ny!=f.npoints) throw std::runtime_error("Template/cropped grid size mismatch");
        f.path=sources.back().path;f.stream=r.stream_index;f.frame=r.frame_index;f.message=r.order;
        fields.push_back(std::move(f));
    }
    std::sort(fields.begin(),fields.end(),[](const Field& a,const Field& b) { return a.message<b.message; });
    return fields;
}
std::vector<double> read_grib(const Field& f) {
    File fp(fopen(f.path.c_str(),"rb"));
    if(!fp || fseeko(fp.get(),static_cast<off_t>(f.offset),SEEK_SET)) throw std::runtime_error("Cannot seek GRIB: "+f.path);
    int err=0;Handle h(codes_handle_new_from_file(nullptr,fp.get(),PRODUCT_GRIB,&err),codes_handle_delete);
    if(!h || point_count(h.get())!=f.npoints) throw std::runtime_error("GRIB field read failed: "+f.path);
    size_t n=f.npoints;std::vector<double> values(n);
    if(codes_get_double_array(h.get(),"values",values.data(),&n) || n!=f.npoints) throw std::runtime_error("Cannot unpack GRIB values");
    long bitmap=0;codes_get_long(h.get(),"bitmapPresent",&bitmap);
    if(bitmap) {
        std::vector<long> mask(n);size_t count=n;
        if(codes_get_long_array(h.get(),"bitmap",mask.data(),&count) || count!=n) throw std::runtime_error("Cannot read GRIB bitmap");
        for(size_t i=0;i<n;++i) if(!mask[i]) values[i]=NAN;
    }
    return values;
}
using Index=std::map<std::string,size_t>;
Index unique_index(const std::vector<Field>& fields,const Options& o,size_t& duplicates,const char* side) {
    Index out;
    for(size_t i=0;i<fields.size();++i) if(!out.emplace(fields[i].identity,i).second) {
        ++duplicates;
        if(!o.first_duplicate) throw std::runtime_error(std::string("Ambiguous duplicate identity in ")+side+": "+fields[i].identity+
                                                       ". Use --duplicate-policy first only if intentional.");
    }
    return out;
}
std::string quote(const std::string& s) {
    std::ostringstream out;out<<'"';
    for(unsigned char c:s) {
        if(c=='"'||c=='\\') out<<'\\'<<c;
        else if(c<32) out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<int>(c)<<std::dec;
        else out<<c;
    }
    out<<'"';return out.str();
}
std::string number(double x) {
    if(!std::isfinite(x)) return "null";
    std::ostringstream out;out<<std::setprecision(17)<<x;return out.str();
}
std::string csv_text(const std::string& s) {
    std::string out="\"";for(char c:s) { if(c=='"') out+='"';out+=c; }return out+'"';
}
struct Result {
    std::string identity,group,short_name,units,ref_path,candidate_path,max_error_identity;
    size_t ref_message=0,candidate_message=0,points_available=0,fields_available=0,fields_selected=0;
    int stream=-1;size_t frame=0;
    uint64_t n=0,examined=0,missing_ref=0,missing_dec=0,mismatch=0,both_missing=0;
    size_t quantile_n=0,max_error_point=0;
    bool quantiles_exact=false;
    std::map<std::string,double> metrics;
    std::map<std::string,std::string> coordinates;
};
Result result(const Accumulator& a) {
    Result r;r.n=a.n;r.examined=a.examined;r.missing_ref=a.missing_reference;r.missing_dec=a.missing_decoded;
    r.mismatch=a.missing_mismatch;r.both_missing=a.both_missing;r.quantile_n=a.reservoir.size();
    r.quantiles_exact=r.quantile_n==r.n;r.max_error_point=a.max_error_point;r.metrics=a.metrics();return r;
}
void write_result(std::ostream& out,const Result& r) {
    out<<"{\"identity\":"<<quote(r.identity)<<",\"group\":"<<quote(r.group)<<",\"short_name\":"<<quote(r.short_name)
       <<",\"units\":"<<quote(r.units)<<",\"reference_path\":"<<quote(r.ref_path)<<",\"candidate_path\":"<<quote(r.candidate_path)
       <<",\"reference_message\":"<<r.ref_message<<",\"candidate_message\":"<<r.candidate_message
       <<",\"stream\":"<<r.stream<<",\"frame\":"<<r.frame<<",\"points_available\":"<<r.points_available
       <<",\"fields_available\":"<<r.fields_available<<",\"fields_selected\":"<<r.fields_selected
       <<",\"n\":"<<r.n<<",\"points_examined\":"<<r.examined<<",\"missing_reference\":"<<r.missing_ref
       <<",\"missing_decoded\":"<<r.missing_dec<<",\"missing_mismatch\":"<<r.mismatch<<",\"both_missing\":"<<r.both_missing
       <<",\"quantile_sample_size\":"<<r.quantile_n<<",\"quantiles_exact\":"<<(r.quantiles_exact?"true":"false")
       <<",\"max_error_identity\":"<<quote(r.max_error_identity)<<",\"max_error_point\":"<<r.max_error_point<<",\"perfect_on_finite_pairs\":"<<(r.n&&r.metrics.at("rmse")==0?"true":"false")
       <<",\"coordinates\":{";
    bool first=true;
    for(const auto& c:r.coordinates) { if(!first) out<<',';first=false;out<<quote(c.first)<<':'<<quote(c.second); }
    out<<"},\"metrics\":{";
    first=true;for(const auto& m:r.metrics) { if(!first) out<<',';first=false;out<<quote(m.first)<<':'<<number(m.second); }
    out<<"}}";
}
void write_csv(std::ostream& out,const std::vector<Result>& results) {
    if(results.empty()) return;
    out<<"identity,group,short_name,units,reference_path,candidate_path,reference_message,candidate_message,stream,frame,"
         "points_available,fields_available,fields_selected,n,points_examined,missing_reference,missing_decoded,missing_mismatch,"
         "both_missing,quantile_sample_size,quantiles_exact,max_error_point";
    const std::vector<std::string> coordinate_keys={"edition","discipline","parameterCategory","parameterNumber","table2Version","indicatorOfParameter","centre","subCentre","tablesVersion","localTablesVersion","paramId","typeOfLevel","level","topLevel","bottomLevel","stepType",
        "md5GridSection","dataDate","dataTime","validityDate","validityTime","startStep","endStep","stepUnits","number","perturbationNumber"};
    for(const auto& key:coordinate_keys) out<<','<<key;
    for(const auto& m:results[0].metrics) out<<','<<m.first;
    out<<'\n';
    for(const auto& r:results) {
        out<<csv_text(r.identity)<<','<<csv_text(r.group)<<','<<csv_text(r.short_name)<<','<<csv_text(r.units)<<','
           <<csv_text(r.ref_path)<<','<<csv_text(r.candidate_path)<<','<<r.ref_message<<','<<r.candidate_message<<','<<r.stream<<','<<r.frame<<','
           <<r.points_available<<','<<r.fields_available<<','<<r.fields_selected<<','<<r.n<<','<<r.examined<<','<<r.missing_ref<<','
           <<r.missing_dec<<','<<r.mismatch<<','<<r.both_missing<<','<<r.quantile_n<<','<<(r.quantiles_exact?"true":"false")<<','<<r.max_error_point;
        for(const auto& key:coordinate_keys) {
            auto it=r.coordinates.find(key);out<<','<<csv_text(it==r.coordinates.end()?"":it->second);
        }
        for(const auto& m:r.metrics) out<<','<<(std::isfinite(m.second)?number(m.second):"");
        out<<'\n';
    }
}
void save(const std::string& path,const std::string& contents) {
    if(path.empty()) return;
    auto temp=path+".tmp."+std::to_string(getpid());
    try {
        std::ofstream out(temp,std::ios::binary);if(!out) throw std::runtime_error("Cannot write: "+temp);
        out<<contents;out.close();if(!out) throw std::runtime_error("Write failed: "+temp);
        fs::rename(temp,path);
    } catch(...) { fs::remove(temp);throw; }
}
void sources_json(std::ostream& out,const std::vector<Source>& sources) {
    out<<'[';bool first=true;
    for(const auto& s:sources) {
        if(!first) out<<',';
        first=false;
        out<<"{\"path\":"<<quote(s.path)<<",\"bytes\":"<<s.bytes<<",\"sha256\":"<<(s.sha.empty()?"null":quote(s.sha))
           <<",\"mtime_ticks\":"<<static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(s.mtime.time_since_epoch()).count())<<'}';
    }
    out<<']';
}
void strings_json(std::ostream& out,const std::vector<std::string>& strings) {
    out<<'[';for(size_t i=0;i<strings.size();++i) { if(i) out<<',';out<<quote(strings[i]); }out<<']';
}
double elapsed(Clock::time_point start) { return std::chrono::duration<double>(Clock::now()-start).count(); }
double peak_mib(int who) {
    struct rusage r{};if(getrusage(who,&r)) return NAN;
#ifdef __APPLE__
    return r.ru_maxrss/(1024.0*1024.0);
#else
    return r.ru_maxrss/1024.0;
#endif
}
} // namespace
int main(int argc,char** argv) {
    if (avenc_print_version(argc, argv)) return 0;
    try {
        auto start=Clock::now();auto o=parse(argc,argv);
        bool mp4=fs::path(o.candidate).extension()==".mp4";
        std::cerr<<"Indexing source identities"<<(o.hash?" and SHA-256":" (hashing disabled)")<<"...\n";
        std::vector<Source> reference_sources,candidate_sources;
        auto refs=index_gribs(o.reference,o,reference_sources);
        auto candidates=mp4?index_mp4(o.candidate,o,candidate_sources):index_gribs(o.candidate,o,candidate_sources);
        std::set<std::string> outputs;
        for(const auto& p:{o.json,o.csv,o.messages_csv}) if(!p.empty()) {
            auto canonical=fs::weakly_canonical(p).string();
            if(!outputs.insert(canonical).second) throw std::runtime_error("Output paths must be distinct");
            for(const auto* sources:{&reference_sources,&candidate_sources}) for(const auto& s:*sources)
                if(canonical==fs::weakly_canonical(s.path).string()) throw std::runtime_error("Output would overwrite an input");
        }
        size_t ref_duplicates=0,candidate_duplicates=0;
        auto ri=unique_index(refs,o,ref_duplicates,"reference"),ci=unique_index(candidates,o,candidate_duplicates,"candidate");
        std::vector<std::string> missing,extra;
        std::map<std::string,std::vector<std::pair<size_t,size_t>>> grouped;
        for(const auto& p:ri) {
            auto it=ci.find(p.first);
            if(it==ci.end()) missing.push_back(p.first);
            else grouped[refs[p.second].group].push_back({p.second,it->second});
        }
        for(const auto& p:ci) if(!ri.count(p.first)) extra.push_back(p.first);
        if((!missing.empty()||!extra.empty())&&!o.allow_unmatched)
            throw std::runtime_error("Field identity mismatch: "+std::to_string(missing.size())+" missing, "+std::to_string(extra.size())+
                                     " extra. First: "+(!missing.empty()?missing.front():extra.front())+". Use --allow-unmatched for an explicit intersection.");
        if(grouped.empty()) throw std::runtime_error("No matched scientific identities");
        struct Pair { size_t ref,candidate; };
        std::vector<Pair> plan;
        std::map<std::string,Accumulator> aggs;
        std::map<std::string,size_t> remaining;
        std::map<std::string,std::string> group_max_identity;
        for(const auto& g:grouped) {
            auto chosen=o.fast?fast_fields(g.second,refs,o.frames,stable_seed(g.first,o.seed)):
                               sample_indices(g.second.size(),o.full?0:o.frames,stable_seed(g.first,o.seed));
            remaining[g.first]=chosen.size();
            for(auto j:chosen) plan.push_back({g.second[j].first,g.second[j].second});
            aggs.emplace(g.first,Accumulator(o.quantile_cap,stable_seed("quantiles:"+g.first,o.seed)));
        }
        std::sort(plan.begin(),plan.end(),[&](const Pair& a,const Pair& b) {
            const auto& x=candidates[a.candidate];const auto& y=candidates[b.candidate];
            if(refs[a.ref].group!=refs[b.ref].group) return refs[a.ref].group<refs[b.ref].group;
            return mp4?std::tie(x.stream,x.frame)<std::tie(y.stream,y.frame):refs[a.ref].identity<refs[b.ref].identity;
        });
        double index_seconds=elapsed(start),reference_read_seconds=0,candidate_read_seconds=0;
        uint64_t decoded_points=0,reference_bytes=0,candidate_bytes=0;
        for(const auto& s:reference_sources) reference_bytes+=s.bytes;
        for(const auto& s:candidate_sources) candidate_bytes+=s.bytes;
        std::vector<Result> fields,groups;fields.reserve(plan.size());
        std::cerr<<"Comparing "<<plan.size()<<" selected / "<<ri.size()-missing.size()<<" matched fields in "<<grouped.size()<<" groups...\n";
        auto process=[&](const Pair& pair,const float* video,const std::vector<double>& grib) {
            const auto& ref=refs[pair.ref];const auto& dec=candidates[pair.candidate];
            auto tick=Clock::now();auto x=read_grib(ref);reference_read_seconds+=elapsed(tick);
            Accumulator a(o.quantile_cap,stable_seed("field-quantiles:"+ref.identity,o.seed));
            auto& agg=aggs.at(ref.group);
            double previous_max=agg.max_abs;
            auto add_point=[&](size_t k) {
                double y=video?static_cast<double>(video[k]):grib[k];
                a.add(x[k],y,k);agg.add(x[k],y,k);
            };
            if(o.full || o.points>=ref.npoints) {
                for(size_t k=0;k<ref.npoints;++k) add_point(k);
            } else {
                auto points=o.fast?stratified_indices(ref.npoints,o.points,stable_seed("points:"+ref.identity,o.seed)):
                                   sample_indices(ref.npoints,o.points,stable_seed("points:"+ref.identity,o.seed));
                for(auto k:points) add_point(k);
            }
            auto r=result(a);r.identity=ref.identity;r.group=ref.group;r.short_name=ref.short_name;r.units=ref.units;
            r.ref_path=ref.path;r.candidate_path=dec.path;r.ref_message=ref.message;r.candidate_message=dec.message;
            r.stream=dec.stream;r.frame=dec.frame;r.points_available=ref.npoints;r.fields_available=1;r.fields_selected=1;
            r.coordinates=ref.coordinates;r.max_error_identity=r.n?ref.identity:"";
            if(agg.n && (!group_max_identity.count(ref.group) || agg.max_abs>previous_max))
                group_max_identity[ref.group]=ref.identity;
            fields.push_back(std::move(r));
            if(--remaining.at(ref.group)==0) {
                auto summary=result(agg);summary.max_error_identity=group_max_identity[ref.group];summary.group=ref.group;summary.short_name=ref.short_name;summary.units=ref.units;
                summary.fields_available=grouped.at(ref.group).size();
                summary.fields_selected=o.full?summary.fields_available:std::min(o.frames,summary.fields_available);
                for(const auto& c:ref.coordinates)
                    if(c.first=="edition" || c.first=="discipline" || c.first=="parameterCategory" ||
                       c.first=="parameterNumber" || c.first=="table2Version" || c.first=="indicatorOfParameter" ||
                       c.first=="centre" || c.first=="subCentre" || c.first=="tablesVersion" ||
                       c.first=="localTablesVersion" || c.first=="paramId" || c.first=="typeOfLevel" || c.first=="level" || c.first=="topLevel" ||
                       c.first=="bottomLevel" || c.first=="stepType" || c.first=="md5GridSection" ||
                       (o.group_member && (c.first=="number" || c.first=="perturbationNumber")))
                        summary.coordinates.insert(c);
                for(const auto& pair:grouped.at(ref.group)) summary.points_available+=refs[pair.first].npoints;
                groups.push_back(std::move(summary));aggs.erase(ref.group);group_max_identity.erase(ref.group);
            }
        };
        for(size_t i=0;i<plan.size();) {
            const auto& f=candidates[plan[i].candidate];
            if(mp4) {
                size_t end=i+1;
                while(end<plan.size()&&end-i<o.batch) {
                    const auto& next=candidates[plan[end].candidate];
                    if(refs[plan[end].ref].group!=refs[plan[i].ref].group || next.stream!=f.stream) break;
                    if(o.fast ? next.frame-f.frame>=o.batch : next.frame!=f.frame+(end-i)) break;
                    ++end;
                }
                const size_t decode_count=candidates[plan[end-1].candidate].frame-f.frame+1;
                float* buffer=nullptr;int w=0,h=0,t=0;
                auto tick=Clock::now();int err=avenc_decode_subset_to_float32(f.path.c_str(),f.stream,0,0,-1,-1,
                    static_cast<int>(f.frame),static_cast<int>(decode_count),&buffer,&w,&h,&t);
                candidate_read_seconds+=elapsed(tick);
                std::unique_ptr<float,decltype(&avenc_free_float32)> decoded(buffer,avenc_free_float32);
                if(err) throw std::runtime_error(std::string("AVENC decode failed: ")+avenc_decode_subset_last_error());
                if(w<=0||h<=0||static_cast<uint64_t>(w)*h!=f.npoints||t!=static_cast<int>(decode_count))
                    throw std::runtime_error("Decoder output shape/frame count mismatch");
                decoded_points+=f.npoints*t;
                for(size_t k=i;k<end;++k)
                    process(plan[k],buffer+(candidates[plan[k].candidate].frame-f.frame)*f.npoints,{});
                i=end;
            } else {
                auto tick=Clock::now();auto y=read_grib(f);candidate_read_seconds+=elapsed(tick);decoded_points+=y.size();
                process(plan[i],nullptr,y);++i;
            }
            if(i==plan.size() || i%50==0) std::cerr<<"  "<<i<<"/"<<plan.size()<<" fields\n";
        }
        for(const auto* sources:{&reference_sources,&candidate_sources}) for(const auto& s:*sources)
            if(s.bytes!=fs::file_size(s.path)||s.mtime!=fs::last_write_time(s.path)) throw std::runtime_error("Input changed during comparison: "+s.path);
        std::map<std::string,std::vector<size_t>> worst;
        for(const std::string criterion:{"rmse","nrmse_q01_q99","max_abs_error","bias","variance_ratio"}) {
            auto& order=worst[criterion];for(size_t i=0;i<fields.size();++i) order.push_back(i);
            auto score=[&](size_t i) {
                double v=fields[i].metrics.at(criterion);
                if(criterion=="bias") v=std::abs(v);
                if(criterion=="variance_ratio") v=std::abs(v-1);
                return std::isfinite(v)?v:-INFINITY;
            };
            std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b) { return score(a)>score(b); });
            if(order.size()>o.top) order.resize(o.top);
        }
        double macro=0;size_t macro_n=0;
        for(const auto& r:groups) { double v=r.metrics.at("nrmse_q01_q99");if(std::isfinite(v)) { macro+=v;++macro_n; } }
        std::ostringstream json;json<<std::setprecision(17);
        json<<"{\"schema\":\"avenc.compare.v1\",\"build\":{\"eccodes_api_version\":"<<codes_get_api_version()
            <<",\"compiler\":"<<quote(__VERSION__)<<"},\"configuration\":{\"mode\":"<<quote(o.full?"full":o.fast?"fast":"stratified")
            <<",\"field_sampling\":"<<quote(o.fast?"one field per chronological stratum, preferring distinct members":"uniform without replacement per group")
            <<",\"point_sampling\":"<<quote(o.fast?"one spatial index per contiguous equal-count scan-order block":"uniform without replacement per field")
            <<",\"seed\":"<<o.seed<<",\"frames_per_group\":"<<o.frames<<",\"points_per_frame\":"<<o.points
            <<",\"quantile_cap\":"<<o.quantile_cap<<",\"decode_batch\":"<<o.batch
            <<",\"group_by_member\":"<<(o.group_member?"true":"false")<<",\"duplicate_policy\":"<<quote(o.first_duplicate?"first":"error")
            <<",\"allow_unmatched\":"<<(o.allow_unmatched?"true":"false")<<",\"hashes_enabled\":"<<(o.hash?"true":"false")
            <<",\"quantile_method\":\"linear interpolation, deterministic reservoir when n exceeds cap; finite pairs only\""
            <<",\"std_ddof\":0,\"error_sign\":\"candidate - original\",\"undefined_metrics\":\"null; includes zero denominators and perfect-reconstruction dB\""
            <<",\"original_values\":\"ecCodes Float64, bitmap missing normalized to NaN\",\"candidate_values\":"
            <<quote(mp4?"AVENC subset API Float32, promoted to Float64":"ecCodes Float64, bitmap missing normalized to NaN")
            <<",\"timing_scope\":\"selected full-field value reads; AVENC includes subprocess and metadata costs; not cold-cache or point-read latency\""
            <<"},\"reference_sources\":";
        sources_json(json,reference_sources);json<<",\"candidate_sources\":";sources_json(json,candidate_sources);
        json<<",\"storage\":{\"reference_bytes\":"<<reference_bytes<<",\"candidate_bytes\":"<<candidate_bytes
            <<",\"grib_to_candidate_ratio\":"<<number(ratio(reference_bytes,candidate_bytes))
            <<"},\"matching\":{\"reference_records\":"<<refs.size()<<",\"candidate_records\":"<<candidates.size()
            <<",\"matched\":"<<ri.size()-missing.size()<<",\"selected\":"<<fields.size()
            <<",\"reference_duplicates\":"<<ref_duplicates<<",\"candidate_duplicates\":"<<candidate_duplicates<<",\"missing\":";
        strings_json(json,missing);json<<",\"extra\":";strings_json(json,extra);
        json<<"},\"timing\":{\"index_and_hash_seconds\":"<<index_seconds<<",\"reference_value_read_seconds\":"<<reference_read_seconds
            <<",\"candidate_value_read_seconds\":"<<candidate_read_seconds<<",\"total_seconds_before_output\":"<<elapsed(start)
            <<",\"candidate_points_decoded\":"<<decoded_points<<",\"peak_parent_rss_mib\":"<<number(peak_mib(RUSAGE_SELF))
            <<",\"largest_child_rss_mib\":"<<number(peak_mib(RUSAGE_CHILDREN))
            <<"},\"global_normalized_summary\":{\"macro_mean_group_nrmse_q01_q99\":"<<number(macro_n?macro/macro_n:NAN)
            <<",\"groups_with_defined_robust_nrmse\":"<<macro_n<<"},\"groups\":[";
        for(size_t i=0;i<groups.size();++i) { if(i) json<<',';write_result(json,groups[i]); }json<<"],\"messages\":[";
        for(size_t i=0;i<fields.size();++i) { if(i) json<<',';write_result(json,fields[i]); }json<<"],\"worst_cases\":{";
        bool first=true;
        for(const auto& w:worst) { if(!first) json<<',';first=false;json<<quote(w.first)<<":[";
            for(size_t i=0;i<w.second.size();++i) { if(i) json<<',';json<<w.second[i]; }json<<']'; }
        json<<"}}\n";
        std::ostringstream csv,messages_csv;write_csv(csv,groups);write_csv(messages_csv,fields);
        save(o.json,json.str());save(o.csv,csv.str());save(o.messages_csv,messages_csv.str());
        std::cout<<"avenc-compare: "<<(o.full?"full":o.fast?"aggressive stratified sample":"stratified sample")<<", seed="<<o.seed
                 <<", matched="<<ri.size()-missing.size()<<", selected="<<fields.size()<<", missing="<<missing.size()<<", extra="<<extra.size()<<'\n'
                 <<"Complete-file ratio: "<<reference_bytes<<" / "<<candidate_bytes<<" = "<<number(ratio(reference_bytes,candidate_bytes))<<"x\n";
        for(const auto& r:groups) {
            std::cout<<"\n"<<r.group<<"\n  n="<<r.n<<"  RMSE="<<number(r.metrics.at("rmse"))<<"  MAE="<<number(r.metrics.at("mae"))
                     <<"  bias="<<number(r.metrics.at("bias"))<<" ["<<r.units<<"]"
                     <<"\n  NRMSE_q01_q99="<<number(r.metrics.at("nrmse_q01_q99"))<<"  SNR_STD="<<number(r.metrics.at("snr_std_db"))
                     <<" dB  std_ratio="<<number(r.metrics.at("std_ratio"))<<"  mask_mismatch="<<r.mismatch
                     <<"  quantiles="<<(r.quantiles_exact?"exact":"reservoir estimate")<<" ("<<r.quantile_n<<")\n";
        }
        std::cout<<"\nValue reads: original="<<reference_read_seconds<<" s, candidate="<<candidate_read_seconds<<" s."
                 <<" Total="<<elapsed(start)<<" s (includes hashing, indexing and metrics).\n";
        for(const auto& w:worst) { std::cout<<"\nWorst fields by "<<w.first<<":\n";
            for(auto i:w.second) std::cout<<"  "<<number(fields[i].metrics.at(w.first))<<"  "<<fields[i].identity<<'\n'; }
        return 0;
    } catch(const std::exception& e) { std::cerr<<"avenc-compare: "<<e.what()<<'\n';return 2; }
}
