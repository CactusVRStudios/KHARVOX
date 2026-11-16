#pragma once
#include "WeaponSupportPolicy.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

namespace kharvox::hands {

struct WeaponSupportProfile {
    bool valid{};
    std::array<std::string,2> assetPaths;
    std::array<std::uint64_t,2> hashes{};
    std::array<float,3> point{};
    std::array<float,3> rightPoint{};
    // v3 geometry is authored at a known physical scale, not weapon scale.
    float metersPerAssetUnit{};
    std::array<float,2> referenceHandScales{};
    std::array<std::uint64_t,2> fistHashes{};
    bool hasHudFrame{};
    std::array<math::Mat4,2> fistToAsset{};
    std::array<int,2> jointSlot{{-1,-1}};
    std::array<math::Mat4,2> jointBindInverse{};
};

// Matches KharvoxWeaponKind without requiring a native hook or game linkage.
inline unsigned weaponSupportKindIndex(const std::string& key) noexcept {
    constexpr std::array<const char*,15> keys{{"unknown","pistol","shotgun",
        "heavy_assault_rifle","plasma_rifle","rocket_launcher","super_shotgun",
        "gauss_cannon","chaingun","bfg","chainsaw","fists","assault_rifle",
        "arc_cannon","mancubus_gland"}};
    for(unsigned index=1;index<keys.size();++index)if(key==keys[index])return index;
    return 0;
}

struct WeaponNativeBinding {
    unsigned kind{}, count{};
    std::array<unsigned,2> indices{};
    std::array<std::string,2> names;
    std::string sourceHash;
    bool capture{};
};

// Optional workshop binding, loaded once; the proven shotgun is immutable.
inline WeaponNativeBinding readWeaponNativeBinding(std::istream& file) {
    WeaponNativeBinding b;
    unsigned version{}, capture{};
    std::string key, extra;
    if(!(file>>version>>key>>b.count>>b.indices[0]>>b.names[0]
        >>b.indices[1]>>b.names[1]>>b.sourceHash>>capture)||version!=1
        ||b.count<2||b.count>128||b.indices[0]>=b.count||b.indices[1]>=b.count
        ||b.sourceHash.size()!=64||capture>1||(file>>extra))return {};
    for(char c:b.sourceHash)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return {};
    b.kind=weaponSupportKindIndex(key);
    if(b.kind==0||b.kind==2)return {};
    b.capture=capture==1;
    return b;
}

using WeaponNativeBindings=std::array<WeaponNativeBinding,15>;
inline WeaponNativeBindings readWeaponNativeBindings(std::istream& file) {
    WeaponNativeBindings bindings{};
    std::string line;
    while(std::getline(file,line)) {
        if(line.find_first_not_of(" \t\r\n")==std::string::npos)continue;
        std::istringstream row(line);
        auto binding=readWeaponNativeBinding(row);
        if(!binding.kind||bindings[binding.kind].kind)return {};
        bindings[binding.kind]=std::move(binding);
    }
    if(file.bad())return {};
    return bindings;
}

namespace support_config_detail {
inline bool relativeAssetPath(const std::string& path) noexcept {
    if(path.empty()||path.front()=='/'||path.front()=='\\')return false;
    std::size_t begin{};
    for(std::size_t i=0;i<=path.size();++i){
        if(i<path.size()){
            const auto c=static_cast<unsigned char>(path[i]);
            if(c<32||c==127||c==':')return false;
            if(c!='/'&&c!='\\')continue;
        }
        if(i-begin==2&&path[begin]=='.'&&path[begin+1]=='.')return false;
        begin=i+1;
    }
    return true;
}
inline bool nonzeroHash(const std::string& text,std::uint64_t& output) noexcept {
    if(text.empty())return false;
    std::uint64_t value{};
    for(unsigned char c:text){
        if(c<'0'||c>'9')return false;
        const auto digit=static_cast<std::uint64_t>(c-'0');
        if(value>(std::numeric_limits<std::uint64_t>::max()-digit)/10)return false;
        value=value*10+digit;
    }
    if(!value)return false;
    output=value;return true;
}
} // namespace support_config_detail

// Startup-only, transactional parsing. Every nonempty line is a complete v1/v2/v3
// profile. Missing or malformed input leaves previously loaded profiles intact.
// Asset identity/file existence is verified separately by the asset loader.
inline std::size_t readWeaponSupportProfiles(std::istream& input,
    std::array<WeaponSupportProfile,15>& output) {
    auto next=output;
    std::array<bool,15> seen{};
    std::size_t count{};
    std::string line;
    while(std::getline(input,line)){
        if(line.find_first_not_of(" \t\r\n")==std::string::npos)continue;
        std::istringstream row(line);row.imbue(std::locale::classic());
        std::string version,key,leftHash,rightHash,extra;
        WeaponSupportProfile profile;
        if(!(row>>version>>key>>profile.assetPaths[0]>>profile.assetPaths[1]
            >>leftHash>>rightHash>>profile.point[0]>>profile.point[1]>>profile.point[2])
            ||(version!="1"&&version!="2"&&version!="3"&&version!="4"&&version!="5"))return 0;
        if(version!="1"){
            if(!(row>>profile.rightPoint[0]>>profile.rightPoint[1]>>profile.rightPoint[2]))return 0;
        }else profile.rightPoint={profile.point[0],-profile.point[1],profile.point[2]};
        if(version=="3"||version=="4"||version=="5"){
            std::string leftFist,rightFist;
            if(!(row>>profile.metersPerAssetUnit>>profile.referenceHandScales[0]
                >>profile.referenceHandScales[1]>>leftFist>>rightFist)
                ||!std::isfinite(profile.metersPerAssetUnit)
                ||profile.metersPerAssetUnit<.000001f||profile.metersPerAssetUnit>1.f
                ||!support_config_detail::nonzeroHash(leftFist,profile.fistHashes[0])
                ||!support_config_detail::nonzeroHash(rightFist,profile.fistHashes[1]))return 0;
            for(float value:profile.referenceHandScales)
                if(!std::isfinite(value)||value<.001f||value>10.f)return 0;
        }
        if(version=="4"||version=="5"){
            for(auto& matrix:profile.fistToAsset){
                for(float& value:matrix.m)if(!(row>>value)||!std::isfinite(value)||std::abs(value)>100000.f)return 0;
                if(std::abs(matrix.m[3])>1e-6f||std::abs(matrix.m[7])>1e-6f
                    ||std::abs(matrix.m[11])>1e-6f||std::abs(matrix.m[15]-1.f)>1e-6f)return 0;
            }
            profile.hasHudFrame=true;
        }
        if(version=="5")for(unsigned hand=0;hand<2;++hand){
            if(!(row>>profile.jointSlot[hand])||profile.jointSlot[hand]<-1||profile.jointSlot[hand]>1)return 0;
            for(float& value:profile.jointBindInverse[hand].m)
                if(!(row>>value)||!std::isfinite(value))return 0;
            math::Mat4 inverse{};
            if(!inverseSupportSimilarity(profile.jointBindInverse[hand],inverse))return 0;
        }
        if(row>>extra)return 0;
        const auto kind=weaponSupportKindIndex(key);
        const bool keepLegacyRight=version!="1"&&profile.assetPaths[1]=="-"&&rightHash=="0";
        if(!kind||seen[kind]
            ||!support_config_detail::relativeAssetPath(profile.assetPaths[0])
            ||!support_config_detail::nonzeroHash(leftHash,profile.hashes[0])
            ||(!keepLegacyRight&&(!support_config_detail::relativeAssetPath(profile.assetPaths[1])
                ||!support_config_detail::nonzeroHash(rightHash,profile.hashes[1]))))return 0;
        for(float value:profile.point)if(!std::isfinite(value)||std::abs(value)>300.f)return 0;
        for(float value:profile.rightPoint)if(!std::isfinite(value)||std::abs(value)>300.f)return 0;
        seen[kind]=true;profile.valid=true;next[kind]=std::move(profile);++count;
    }
    if(input.bad()||!count)return 0;
    output=std::move(next);return count;
}

} // namespace kharvox::hands
