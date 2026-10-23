#include "../src/avenc_compare_metrics.h"
#include "../src/avenc_compare_sha256.h"
#include <iostream>
#include <stdexcept>
using namespace avenc_compare;
void require(bool ok,const char* msg) { if(!ok) throw std::runtime_error(msg); }
void near(double a,double b) { require(std::abs(a-b)<1e-10,"numeric mismatch"); }
int main() {
    Accumulator a(0);for(int i=1;i<=4;++i) a.add(i,i+2,i-1);
    a.add(NAN,1,4);a.add(2,NAN,5);a.add(NAN,NAN,6);
    auto m=a.metrics();near(m.at("rmse"),2);near(m.at("mae"),2);near(m.at("bias"),2);
    near(m.at("rmse_over_std"),2/std::sqrt(1.25));near(m.at("max_over_std"),2/std::sqrt(1.25));
    near(m.at("error_std"),0);near(m.at("correlation"),1);near(m.at("std_ratio"),1);
    near(m.at("reference_std"),std::sqrt(1.25));near(m.at("nrmse_range"),2./3);
    near(m.at("q05_shift"),2);near(m.at("nrmse_q01_q99"),2./2.94);
    require(a.n==4 && a.examined==7 && a.missing_mismatch==2 && a.both_missing==1,"mask counts");
    Accumulator constant;constant.add(280,280,0);
    require(std::isnan(constant.metrics().at("nrmse_range")),"zero denominator");
    require(std::isnan(constant.metrics().at("rmse_over_std")),"undefined normalized RMSE");
    require(std::isnan(constant.metrics().at("max_over_std")),"undefined normalized maximum");
    Accumulator large(0);for(int i=0;i<4;++i) large.add(1e12+i,1e12+i+1,i);
    near(large.metrics().at("reference_std"),std::sqrt(1.25));
    Accumulator r(8,42),s(8,42);for(int i=0;i<100;++i) { r.add(i,i+.5,i);s.add(i,i+.5,i); }
    require(r.reservoir==s.reservoir && r.reservoir.size()==8,"reservoir determinism");
    auto p=sample_indices(100,11,5489);require(p==sample_indices(100,11,5489),"sampling determinism");
    require(p.size()==11 && std::adjacent_find(p.begin(),p.end())==p.end(),"sample uniqueness");
    auto strata=stratified_indices(103,10,42);
    require(strata==stratified_indices(103,10,42),"stratified reproducibility");
    require(strata.size()==10 && std::adjacent_find(strata.begin(),strata.end())==strata.end(),"stratified uniqueness");
    require(strata.front()<11 && strata.back()>=93,"spatial coverage");
    require(stratified_indices(3,20,42)==std::vector<size_t>({0,1,2}),"small fields use all points");
    Sha256 hash;const std::string abc="abc";hash.update(reinterpret_cast<const unsigned char*>(abc.data()),abc.size());
    require(hash.finish()=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc");
    Sha256 empty;require(empty.finish()=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA256 empty");
    std::cout<<"metric, mask, sampling and SHA-256 checks passed\n";
}
