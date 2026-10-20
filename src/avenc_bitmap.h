// GRIB bitmap fingerprints and optional three-cell encoding halo.
// MD5 is a deduplication fingerprint, never a security check. RFC 1321.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <eccodes.h>

namespace avenc_bitmap {
inline std::string md5(const std::vector<uint8_t>& input) {
    static constexpr uint32_t k[64] = {
        0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
        0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
        0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
        0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
        0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
        0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
        0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
        0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u};
    static constexpr unsigned shifts[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    std::vector<uint8_t> bytes(input);
    const uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8;
    bytes.push_back(0x80);
    while (bytes.size() % 64 != 56) bytes.push_back(0);
    for (unsigned i=0; i<8; ++i) bytes.push_back(static_cast<uint8_t>(bits >> (8*i)));
    uint32_t a0=0x67452301u, b0=0xefcdab89u, c0=0x98badcfeu, d0=0x10325476u;
    for (size_t off=0; off<bytes.size(); off+=64) {
        uint32_t w[16]{};
        for (unsigned i=0; i<64; ++i) w[i/4] |= uint32_t(bytes[off+i]) << (8*(i%4));
        uint32_t a=a0,b=b0,c=c0,d=d0;
        for (unsigned i=0; i<64; ++i) {
            uint32_t f,g;
            if (i<16) { f=(b&c)|(~b&d); g=i; }
            else if (i<32) { f=(d&b)|(~d&c); g=(5*i+1)%16; }
            else if (i<48) { f=b^c^d; g=(3*i+5)%16; }
            else { f=c^(b|~d); g=(7*i)%16; }
            const uint32_t x=a+f+k[i]+w[g];
            a=d; d=c; c=b; b+=(x<<shifts[i])|(x>>(32-shifts[i]));
        }
        a0+=a;b0+=b;c0+=c;d0+=d;
    }
    std::ostringstream out; out<<std::hex<<std::setfill('0');
    for (auto x : {a0,b0,c0,d0})
        for (unsigned i=0;i<4;++i) out<<std::setw(2)<<((x>>(8*i))&255u);
    return out.str();
}

// GRIB bits: one = valid. Exclude section headers and canonicalize padding bits.
// ecCodes resolves the bitmap accessor; fail rather than infer it from values.
inline std::vector<uint8_t> read(codes_handle* h, size_t pixels) {
    size_t count=0;
    if (codes_get_size(h,"bitmap",&count) != 0 || count != pixels)
        throw std::runtime_error("GRIB bitmap size does not match its grid");
    const size_t expected=(pixels+7)/8;
    std::vector<uint8_t> packed(expected+1,0);
    size_t size=packed.size();
    if (codes_get_bytes(h,"bitmap",packed.data(),&size) != 0 || size < expected) {
        // Fallback for accessors that expose resolved bits but not raw bytes.
        std::vector<long> bits(pixels);
        count=pixels;
        if (codes_get_long_array(h,"bitmap",bits.data(),&count) != 0 || count != pixels)
            throw std::runtime_error("Unable to read the actual GRIB bitmap");
        std::fill(packed.begin(),packed.end(),0);
        for (size_t i=0;i<pixels;++i) if (bits[i]) packed[i/8] |= 0x80u>>(i%8);
    }
    packed.resize(expected);
    if (pixels%8) packed.back() &= static_cast<uint8_t>(0xffu << (8-pixels%8));
    return packed;
}
inline bool missing(const std::vector<uint8_t>& packed, size_t i) {
    return !packed.empty() && !(packed[i/8] & (0x80u>>(i%8)));
}
inline std::vector<uint8_t> expand(const std::vector<uint8_t>& packed,size_t pixels) {
    std::vector<uint8_t> result(pixels);
    for (size_t i=0;i<pixels;++i) result[i]=missing(packed,i)?1:0;
    return result;
}

struct HaloCell { size_t index, source; unsigned ring; };
// Deterministic 8-neighbour (Chebyshev distance) outward propagation, no wrapping.
// Only missing cells within three grid cells of valid data are retained.
// Ties select the smallest row-major valid source index.
inline std::vector<HaloCell> halo3(const std::vector<uint8_t>& packed,int nx,int ny) {
    const size_t pixels=size_t(nx)*ny;
    const size_t unset=pixels;
    std::vector<size_t> sources(pixels,unset);
    std::vector<HaloCell> halo;
    std::vector<size_t> frontier;
    for (unsigned ring=1;ring<=3;++ring) {
        std::vector<HaloCell> next;
        auto visit = [&](size_t i) {
            if (!missing(packed,i) || sources[i]!=unset) return;
            size_t best=unset;
            const int x=int(i%nx),y=int(i/nx);
            for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
                const int xx=x+dx, yy=y+dy;
                if (xx<0 || xx>=nx || yy<0 || yy>=ny) continue;
                const size_t j=size_t(yy)*nx+xx;
                if (!missing(packed,j)) best=std::min(best,j);
                else if (sources[j]!=unset) best=std::min(best,sources[j]);
            }
            if (best!=unset) next.push_back({i,best,ring});
        };
        if (ring==1) {
            for (size_t i=0;i<pixels;++i) visit(i);
        } else {
            std::vector<size_t> candidates;
            for (size_t i:frontier) {
                const int x=int(i%nx),y=int(i/nx);
                for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
                    const int xx=x+dx,yy=y+dy;
                    if (xx>=0 && xx<nx && yy>=0 && yy<ny) candidates.push_back(size_t(yy)*nx+xx);
                }
            }
            std::sort(candidates.begin(),candidates.end());
            candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
            for (size_t i:candidates) visit(i);
        }
        frontier.clear();
        for (const auto& cell:next) { sources[cell.index]=cell.source;frontier.push_back(cell.index); }
        halo.insert(halo.end(),next.begin(),next.end());
    }
    return halo;
}
template<class T> inline void taper(std::vector<T>& q,const std::vector<HaloCell>& halo) {
    // Minimum filler remains code zero, as in the historical min policy.
    // Source cells are always valid, so no filled value is recursively sampled.
    for (const auto& cell:halo)
        q[cell.index]=static_cast<T>((uint64_t(q[cell.source])*(4-cell.ring)+1)/3);
}
} // namespace avenc_bitmap
