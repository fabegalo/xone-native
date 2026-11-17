// SPDX-License-Identifier: BSD-3-Clause
#include "era_shader_translation.h"
#include <dxgi.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <span>

namespace xboxone::era {
namespace {
using Words=std::vector<std::uint32_t>;
// Reference 14059ea50/14059ec10: MD5 compression with the DXBC final block layout.
void checksum(std::vector<std::uint8_t>& code) {
    constexpr std::uint32_t constants[]{
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    constexpr unsigned rotations[][4]{{7,12,17,22},{5,9,14,20},{4,11,16,23},{6,10,15,21}};
    std::array<std::uint32_t,4> state{0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    const auto block=[&](const void* data) {
        std::array<std::uint32_t,16> words{};std::memcpy(words.data(),data,64);
        auto a=state[0],b=state[1],c=state[2],d=state[3];
        for(unsigned i=0;i<64;++i) {
            const auto f=i<16 ? (b&c)|(~b&d) : i<32 ? (d&b)|(~d&c) : i<48 ? b^c^d : c^(b|~d);
            const auto at=i<16 ? i : i<32 ? (5*i+1)%16 : i<48 ? (3*i+5)%16 : (7*i)%16;
            const auto next=b+std::rotl(a+f+constants[i]+words[at],int(rotations[i/16][i%4]));
            a=d;d=c;c=b;b=next;
        }
        state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
    };
    const SIZE_T bytes=code.size()-20,full=bytes&~SIZE_T{63};
    for(SIZE_T at=0;at<full;at+=64)block(code.data()+20+at);
    std::array<std::uint8_t,64> tail{};const SIZE_T left=bytes-full;
    const auto bits=std::uint32_t(bytes*8);
    if(left<56) {std::memcpy(tail.data(),&bits,4);std::memcpy(tail.data()+4,code.data()+20+full,left);tail[left+4]=0x80;}
    else {std::memcpy(tail.data(),code.data()+20+full,left);tail[left]=0x80;block(tail.data());tail={};std::memcpy(tail.data(),&bits,4);}
    const auto last=(bits|4u)>>2;std::memcpy(tail.data()+60,&last,4);block(tail.data());
    std::memcpy(code.data()+4,state.data(),16);
}
bool operand(std::span<const std::uint32_t> words,SIZE_T& at,unsigned depth=0) {
    if(at>=words.size() || depth>8)return false;
    const auto token=words[at++];auto extension=token;
    while(extension&0x80000000u){if(at>=words.size())return false;extension=words[at++];}
    const unsigned components=token&3,type=(token>>12)&255,dimensions=(token>>20)&3;
    if(type==4 || type==5) {
        if(dimensions || (components!=1 && components!=2))return false;
        const SIZE_T n=(components==1?1:4)*(type==5?2:1);if(n>words.size()-at)return false;at+=n;return true;
    }
    for(unsigned i=0;i<dimensions;++i) {
        const auto representation=(token>>(22+3*i))&7;
        const SIZE_T n=representation==0 || representation==3 ? 1 : representation==1 || representation==4 ? 2 : 0;
        if(representation>4 || n>words.size()-at)return false;at+=n;
        if(representation>=2 && !operand(words,at,depth+1))return false;
    }
    return components!=3;
}
Words absolute(Words value) {
    if(((value[0]>>12)&255)==4) {
        SIZE_T at=1;while(value[at-1]&0x80000000u)++at;
        for(;at<value.size();++at)value[at]&=0x7fffffffu;
        return value;
    }
    SIZE_T at=1;auto previous=value[0];
    while(previous&0x80000000u) {
        if((value[at]&63)==1){value[at]=(value[at]&~0x3fc0u)|0x80u;return value;}
        previous=value[at++];
    }
    const bool extended=(value[0]&0x80000000u)!=0;value[0]|=0x80000000u;
    value.insert(value.begin()+1,0x81u|(extended?0x80000000u:0));return value;
}
Words destination(UINT index){return {0x001000f2,index};}
Words source(UINT index){return {0x00100e46,index};}
Words immediate(UINT value){return {0x00004002,value,value,value,value};}
bool emit(Words& out,UINT opcode,std::initializer_list<Words> operands,UINT flags=0) {
    SIZE_T size=1;for(const auto& op:operands)size+=op.size();if(size>127)return false;
    out.push_back(opcode|flags|UINT(size<<24));for(const auto& op:operands)out.insert(out.end(),op.begin(),op.end());return true;
}
HRESULT program(Words& words) {
    if(words.size()<2 || words[1]!=words.size())return E_INVALIDARG;
    UINT temps=0;bool has_temps=false,changed=false;
    for(SIZE_T at=2;at<words.size();) {
        const auto op=words[at]&0x7ff;const auto size=op==53 && at+1<words.size() ? words[at+1] : (words[at]>>24)&127;
        if(!size || size>words.size()-at)return E_INVALIDARG;
        if(op==104){if(size!=2)return E_INVALIDARG;temps=words[at+1];has_temps=true;}
        if(op==14 || (op>=15&&op<=17) || op==50 || op==56)changed=true;
        at+=size;
    }
    if(!changed)return S_FALSE;if(temps>4093)return DXGI_ERROR_UNSUPPORTED;
    Words output{words[0],0};
    for(SIZE_T at=2;at<words.size();) {
        const auto token=words[at],op=token&0x7ff;const SIZE_T size=op==53 ? words[at+1] : (token>>24)&127;
        if(!has_temps && op!=106){emit(output,104,{{temps+3}});has_temps=true;}
        if(op==104){output.push_back(token);output.push_back(temps+3);}
        else if(op==14 || (op>=15&&op<=17) || op==50 || op==56) {
            SIZE_T cursor=1;if(token&0x80000000u)return DXGI_ERROR_UNSUPPORTED;
            std::array<Words,4> args;const unsigned count=op==50?4:3;
            const std::span<const UINT> instruction(words.data()+at,size);
            for(unsigned i=0;i<count;++i){const auto begin=cursor;if(!operand(instruction,cursor))return E_INVALIDARG;args[i].assign(instruction.begin()+begin,instruction.begin()+cursor);}
            if(cursor!=size)return E_INVALIDARG;
            const auto result=destination(temps),condition=destination(temps+1),r=source(temps),c=source(temps+1),zero=immediate(0);
            const UINT flags=token&0x00fff800u,unsaturated=flags&~0x2000u;
            // The reference preserves legacy zero products, then reduces dots/adds the MAD addend.
            if(!emit(output,op==14?14:56,{result,args[1],args[2]},unsaturated) ||
               !(op==14 ? emit(output,24,{condition,args[1],zero}) :
                 emit(output,51,{condition,absolute(args[1]),absolute(args[2])}) && emit(output,24,{condition,c,zero})) ||
               !emit(output,55,{result,c,zero,r}))return DXGI_ERROR_UNSUPPORTED;
            if(op>=15&&op<=17){if(!emit(output,op,{args[0],r,immediate(0x3f800000)},flags))return DXGI_ERROR_UNSUPPORTED;}
            else if(op==50){if(!emit(output,0,{args[0],r,args[3]},flags))return DXGI_ERROR_UNSUPPORTED;}
            else if(!emit(output,54,{args[0],r},flags))return DXGI_ERROR_UNSUPPORTED;
        } else output.insert(output.end(),words.begin()+at,words.begin()+at+size);
        if(output.size()>(16u<<20))return DXGI_ERROR_UNSUPPORTED;
        at+=size;
    }
    output[1]=UINT(output.size());words=std::move(output);return S_OK;
}
}
HRESULT translate_reference_shader(std::vector<std::uint8_t>& code,const char* creator) {
    if(creator && std::strcmp(creator,"Microsoft (R) HLSL Shader Compiler 9.30.9821.0")==0)return S_FALSE;
    if(code.size()<32 || code.size()>(16u<<20))return E_INVALIDARG;
    Words container((code.size()+3)/4);std::memcpy(container.data(),code.data(),code.size());
    if(container[0]!=0x43425844 || container[6]!=code.size() || container[7]>(code.size()-32)/4)return E_INVALIDARG;
    std::vector<std::uint8_t> result(code.begin(),code.begin()+32+container[7]*4);bool changed=false;
    for(UINT i=0;i<container[7];++i) {
        const SIZE_T offset=container[8+i];if(offset%4 || offset<32+container[7]*4 || offset>code.size()-8)return E_INVALIDARG;
        const auto kind=container[offset/4],size=container[offset/4+1];if(size>code.size()-offset-8)return E_INVALIDARG;
        if(kind!=0x52444853 && kind!=0x58454853) {
            const auto new_offset=UINT(result.size());std::memcpy(result.data()+32+i*4,&new_offset,4);
            result.insert(result.end(),code.begin()+offset,code.begin()+offset+8+size);
            while(result.size()%4)result.push_back(0);
            continue;
        }
        if(size%4)return E_INVALIDARG;
        Words chunk(container.begin()+offset/4+2,container.begin()+(offset+8+size)/4);
        if(kind==0x52444853 || kind==0x58454853){const HRESULT hr=program(chunk);if(FAILED(hr))return hr;changed|=hr==S_OK;}
        const auto new_offset=UINT(result.size()),new_size=UINT(chunk.size()*4);std::memcpy(result.data()+32+i*4,&new_offset,4);
        const auto previous=result.size();result.resize(previous+8+new_size);std::memcpy(result.data()+previous,&kind,4);std::memcpy(result.data()+previous+4,&new_size,4);std::memcpy(result.data()+previous+8,chunk.data(),new_size);
        if(result.size()>(64u<<20))return DXGI_ERROR_UNSUPPORTED;
    }
    if(!changed)return S_FALSE;const auto bytes=UINT(result.size());std::memcpy(result.data()+24,&bytes,4);checksum(result);code=std::move(result);return S_OK;
}
}
