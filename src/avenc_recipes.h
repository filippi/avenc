#pragma once
// Shared CSV contract for evaluator output and native stream overrides.
#include <eccodes.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace avenc_recipes {
inline const std::vector<std::string>& selectors() {
    static const std::vector<std::string> names={"paramId","shortName","typeOfLevel","level","stepType","stepUnits","md5GridSection","number","perturbationNumber","topLevel","bottomLevel","edition","discipline","parameterCategory","parameterNumber","table2Version","indicatorOfParameter","centre","subCentre","tablesVersion","localTablesVersion"};
    return names;
}
inline std::string value(codes_handle* h,const std::string& key) {
    int error=0;if(codes_is_missing(h,key.c_str(),&error)||error)return "";
    size_t n=0;if(codes_get_length(h,key.c_str(),&n))return "";
    std::vector<char> b(n+2);n=b.size();if(codes_get_string(h,key.c_str(),b.data(),&n))return "";return b.data();
}
inline std::string cell(const std::string& s) {std::string out="\"";for(char c:s){if(c=='"')out+='"';out+=c;}return out+'"';}
inline std::vector<std::vector<std::string>> read(const std::string& path) {
    std::ifstream in(path);if(!in)throw std::runtime_error("Cannot open recipes: "+path);
    std::vector<std::vector<std::string>> rows;std::vector<std::string> row;std::string cell;bool quoted=false;char c;
    while(in.get(c)){
        if(c=='"'){if(quoted&&in.peek()=='"'){cell+='"';in.get();}else quoted=!quoted;}
        else if(c==','&&!quoted){row.push_back(cell);cell.clear();}
        else if(c=='\n'&&!quoted){row.push_back(cell);cell.clear();rows.push_back(row);row.clear();}
        else if(c!='\r'||quoted)cell+=c;
    }
    if(quoted)throw std::runtime_error("Unclosed quote in recipes CSV");
    if(!cell.empty()||!row.empty()){row.push_back(cell);rows.push_back(row);}return rows;
}
struct Rule {std::map<std::string,std::string> match,options;size_t line=0;};
class Table {
public:
    std::vector<Rule> rules;
    explicit Table(const std::string& path) {
        if(path.empty())return;
        const auto rows=read(path);if(rows.empty())throw std::runtime_error("Empty recipes CSV");
        const std::set<std::string> settings={"codec","quality","encoder","bits","quantization","transform","transform_parameter","group_by","gop","crf","chroma","encoder_preset","svtav1_params","action"};
        std::set<std::string> seen;
        for(const auto& name:rows[0]){
            if(!seen.insert(name).second)throw std::runtime_error("Duplicate recipe column: "+name);
            if(!settings.count(name)&&std::find(selectors().begin(),selectors().end(),name)==selectors().end())throw std::runtime_error("Unknown recipe column: "+name);
        }
        for(size_t i=1;i<rows.size();++i){
            if(rows[i].size()==1&&rows[i][0].empty())continue;
            if(rows[i].size()!=rows[0].size())throw std::runtime_error("Wrong recipe column count at line "+std::to_string(i+1));
            Rule r;r.line=i+1;
            for(size_t j=0;j<rows[i].size();++j)if(!rows[i][j].empty())(settings.count(rows[0][j])?r.options:r.match)[rows[0][j]]=rows[i][j];
            if(r.match.empty())throw std::runtime_error("Recipe must have at least one scientific selector");
            rules.push_back(std::move(r));
        }
    }
    // More-specific selector sets override subsets. Incomparable matches conflict.
    int match(codes_handle* h) const {
        std::map<std::string,std::string> cache;std::vector<int> matching;
        for(size_t i=0;i<rules.size();++i){bool yes=true;for(const auto& kv:rules[i].match){auto it=cache.find(kv.first);if(it==cache.end())it=cache.emplace(kv.first,value(h,kv.first)).first;if(it->second!=kv.second){yes=false;break;}}if(yes)matching.push_back(int(i));}
        int best=-1;
        for(int i:matching){bool dominated=false;for(int j:matching)if(i!=j&&rules[j].match.size()>rules[i].match.size()){
            bool subset=true;for(const auto& kv:rules[i].match){auto it=rules[j].match.find(kv.first);if(it==rules[j].match.end()||it->second!=kv.second){subset=false;break;}}if(subset){dominated=true;break;}
        }if(!dominated){if(best>=0)throw std::runtime_error("Conflicting matching recipe lines "+std::to_string(rules[best].line)+" and "+std::to_string(rules[i].line));best=i;}}
        return best;
    }
};
} // namespace avenc_recipes
