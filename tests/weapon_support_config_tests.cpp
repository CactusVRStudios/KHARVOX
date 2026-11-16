#include "../src/hands/WeaponSupportConfig.h"
#include <cstdlib>
#include <fstream>

using namespace kharvox::hands;
namespace {
using Profiles=std::array<WeaponSupportProfile,15>;
void check(bool value){if(!value)std::abort();}
bool same(const Profiles& a,const Profiles& b) {
    for(std::size_t i=0;i<a.size();++i)if(a[i].valid!=b[i].valid
        ||a[i].assetPaths!=b[i].assetPaths||a[i].hashes!=b[i].hashes||a[i].point!=b[i].point
        ||a[i].rightPoint!=b[i].rightPoint||a[i].metersPerAssetUnit!=b[i].metersPerAssetUnit
        ||a[i].referenceHandScales!=b[i].referenceHandScales||a[i].fistHashes!=b[i].fistHashes
        ||a[i].hasHudFrame!=b[i].hasHudFrame)return false;
    for(std::size_t i=0;i<a.size();++i)for(int hand=0;hand<2;++hand)for(int n=0;n<16;++n)
        if(a[i].fistToAsset[hand].m[n]!=b[i].fistToAsset[hand].m[n])return false;
    return true;
}
std::string record(const std::string& key="shotgun",const std::string& left="assets/3D/left.glb",
    const std::string& right="assets/3D/right.glb",const std::string& first="1",
    const std::string& second="18446744073709551615",const std::string& xyz="1 -2 3") {
    return "1 "+key+" "+left+" "+right+" "+first+" "+second+" "+xyz+"\n";
}
void reject(const std::string& text,const Profiles& original) {
    auto profiles=original;std::istringstream input(text);
    check(readWeaponSupportProfiles(input,profiles)==0&&same(profiles,original));
}
}

int main(int argc,char** argv) {
    {
        const std::string hash(64,'a');
        const std::string super="1 super_shotgun 11 2 hinge 1 base "+hash+" 0\n";
        const std::string heavy="1 heavy_assault_rifle 36 1 base_jnt 1 base_jnt "+hash+" 0\n";
        std::istringstream both(super+heavy);
        const auto loaded=readWeaponNativeBindings(both);
        check(loaded[6].kind==6&&loaded[6].indices==std::array<unsigned,2>{2,1});
        check(loaded[3].kind==3&&loaded[3].indices==std::array<unsigned,2>{1,1});
        check(!loaded[2].kind&&!loaded[6].capture&&!loaded[3].capture);
        std::istringstream legacy(super);
        check(readWeaponNativeBindings(legacy)[6].indices==loaded[6].indices);
        for(const auto& bad:{super+super,super+"invalid\n",super+"1 shotgun 11 3 pump 1 base "+hash+" 0\n"}) {
            std::istringstream input(bad);const auto rejected=readWeaponNativeBindings(input);
            check(!rejected[3].kind&&!rejected[6].kind&&!rejected[2].kind);
        }
    }
    {
        std::string identity="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1";
        std::istringstream native("5 shotgun assets/l.glb assets/r.glb 1 2 11 0 0 -5 0 -2 .0254 .2464 .2464 3 4 "
            +identity+" "+identity+" 0 "+identity+" -1 "+identity+"\n");
        Profiles loaded{};check(readWeaponSupportProfiles(native,loaded)==1);
        check(loaded[2].jointSlot[0]==0&&loaded[2].jointSlot[1]==-1&&loaded[2].hasHudFrame);
    }
    check(argc>=1&&argc<=3);
    const std::array<const char*,14> keys{{"pistol","shotgun","heavy_assault_rifle",
        "plasma_rifle","rocket_launcher","super_shotgun","gauss_cannon","chaingun",
        "bfg","chainsaw","fists","assault_rifle","arc_cannon","mancubus_gland"}};
    for(unsigned i=0;i<keys.size();++i)check(weaponSupportKindIndex(keys[i])==i+1);
    check(weaponSupportKindIndex("unknown")==0&&weaponSupportKindIndex("Gauss_Cannon")==0);

    Profiles profiles{};
    std::istringstream good(record()+record("pistol","hands\\left.glb","hands\\right.glb",
        "18446744073709551615","2","300 -300 0"));
    check(readWeaponSupportProfiles(good,profiles)==2);
    check(profiles[2].valid&&profiles[2].point==std::array<float,3>{1,-2,3});
    check(profiles[2].rightPoint==std::array<float,3>{1,2,3});
    check(profiles[2].hashes[1]==std::numeric_limits<std::uint64_t>::max());
    check(profiles[1].valid&&profiles[1].assetPaths[0]=="hands\\left.glb"
        &&profiles[1].point==std::array<float,3>{300,-300,0});
    check(!profiles[0].valid&&!profiles[7].valid);
    const auto original=profiles;
    const std::string physical="3 shotgun assets/l.glb assets/r.glb 1 2 22 0 -2 22 0 -2 .0254 .2464 .2464 123 456\n";
    std::istringstream v3(physical);
    check(readWeaponSupportProfiles(v3,profiles)==1&&profiles[2].metersPerAssetUnit==.0254f
        &&profiles[2].referenceHandScales[1]==.2464f&&profiles[2].fistHashes[1]==456);
    for(const auto* tail:{"0 .2464 .2464 123 456","nan .2464 .2464 123 456",
        ".0254 0 .2464 123 456",".0254 .2464 inf 123 456",
        ".0254 .2464 .2464 0 456",".0254 .2464 .2464 123",".0254 .2464 .2464 123 456 extra"})
        reject(std::string("3 shotgun assets/l.glb assets/r.glb 1 2 22 0 -2 22 0 -2 ")+tail+"\n",original);
    profiles=original;
    std::string wrist=physical;wrist[0]='4';wrist.pop_back();
    wrist+=" 1 0 0 0 0 1 0 0 0 0 1 0 2 3 4 1";
    reject(wrist+"\n",original); // missing the second hand's matrix
    wrist+=" 1 0 0 0 0 1 0 0 0 0 1 0 5 6 7 1\n";
    std::istringstream v4(wrist);check(readWeaponSupportProfiles(v4,profiles)==1
        &&profiles[2].hasHudFrame&&profiles[2].fistToAsset[1].m[12]==5);
    profiles=original;
    std::istringstream next(record("gauss_cannon"));
    check(readWeaponSupportProfiles(next,profiles)==1&&profiles[1].valid&&profiles[2].valid&&profiles[7].valid);
    profiles=original;

    reject("",original);reject(" \t\r\n\n",original);
    reject(record()+record(),original);
    reject(record("unsupported"),original);reject(record("unknown"),original);
    reject("3 shotgun assets/l.glb assets/r.glb 1 2 0 0 0\n",original);
    reject("2 shotgun assets/l.glb assets/r.glb 1 2 0 0 0\n",original);
    reject(record()+"1 bfg truncated\n",original); // do not partially publish the first row
    reject(record()+"garbage",original);reject(record()+"extra\n",original);
    for(const auto* xyz:{"nan 0 0","0 inf 0","0 0 -inf","300.01 0 0","0 -301 0","0 0 1e100"})
        reject(record("shotgun","assets/l.glb","assets/r.glb","1","2",xyz),original);
    for(const auto* hash:{"0","-1","+1","1x","18446744073709551616"}){
        reject(record("shotgun","assets/l.glb","assets/r.glb",hash,"2"),original);
        reject(record("shotgun","assets/l.glb","assets/r.glb","1",hash),original);
    }
    for(const auto* path:{"/absolute.glb","\\rooted.glb","C:\\hands.glb","C:hands.glb",
        "\\\\server\\hands.glb","../hands.glb","assets/../hands.glb","assets\\..\\hands.glb",
        "assets/hands.glb:stream","assets/.."}){
        reject(record("shotgun",path),original);
        reject(record("shotgun","assets/l.glb",path),original);
    }
    reject(record("shotgun",std::string("assets/evil\0.glb",16)),original);
    std::istringstream missing;missing.setstate(std::ios::failbit);
    check(readWeaponSupportProfiles(missing,profiles)==0&&same(profiles,original));
    std::istringstream failed(record());failed.setstate(std::ios::badbit);
    check(readWeaponSupportProfiles(failed,profiles)==0&&same(profiles,original));

    const std::string explicitRight="2 gauss_cannon assets/l.glb assets/r.glb 1 2 1.677667 32.5 4.280817 1.677667 36.40428 4.280817\n";
    std::istringstream version2(explicitRight);
    check(readWeaponSupportProfiles(version2,profiles)==1&&profiles[7].valid
        &&profiles[7].point==std::array<float,3>{1.677667f,32.5f,4.280817f}
        &&profiles[7].rightPoint==std::array<float,3>{1.677667f,36.40428f,4.280817f});
    check(profiles[1].rightPoint==original[1].rightPoint);
    reject(explicitRight+explicitRight,original);
    reject(explicitRight.substr(0,explicitRight.size()-1)+" extra\n",original);
    reject(record().substr(0,record().size()-1)+" 1 2 3\n",original); // v1 cannot contain v2 fields
    for(const auto* xyz:{"nan 0 0","0 inf 0","0 0 -inf","301 0 0","0 -300.01 0","0 0 1e100"})
        reject(std::string("2 shotgun assets/l.glb assets/r.glb 1 2 1 2 3 ")+xyz+"\n",original);
    reject("2 shotgun assets/l.glb assets/r.glb 1 2 1 2 3 4 5\n",original);
    std::istringstream keepLegacy("2 gauss_cannon assets/l.glb - 1 0 1 32 4 1 32 4\n");
    check(readWeaponSupportProfiles(keepLegacy,profiles)==1&&profiles[7].valid
        &&profiles[7].hashes[1]==0&&profiles[7].assetPaths[1]=="-");
    reject("2 shotgun assets/l.glb assets/r.glb 1 0 1 2 3 4 5 6\n",original);
    reject("1 shotgun assets/l.glb - 1 0 1 2 3\n",original);

    if(argc>=2){
        std::ifstream fixture(argv[1]);check(bool(fixture));
        const bool pumps=argc==3&&std::string(argv[2])=="2";
        const bool workshop=argc==3&&std::string(argv[2])=="3";
        const bool plasma=argc==3&&std::string(argv[2])=="4";
        const bool rocket=argc==3&&std::string(argv[2])=="5";
        const bool gauss=argc==3&&std::string(argv[2])=="6";
        const bool chaingun=argc==3&&std::string(argv[2])=="7";
        const bool bfg=argc==3&&std::string(argv[2])=="8";
        const bool pistol=argc==3&&std::string(argv[2])=="10";
        check(argc!=3||pumps||workshop||plasma||rocket||gauss||chaingun||bfg||pistol);
        Profiles loaded{};check(readWeaponSupportProfiles(fixture,loaded)==(pumps?2:workshop?3:plasma?4:rocket?5:gauss?6:chaingun?7:bfg?8:pistol?10:9));
        for(unsigned kind=pistol?1:2;kind<=10;++kind){
            if((pumps||workshop||plasma||rocket||gauss||chaingun||bfg)&&kind!=2&&kind!=6&&!((workshop||plasma||rocket||gauss||chaingun||bfg)&&kind==3)&&!((plasma||rocket||gauss||chaingun||bfg)&&kind==4)&&!((rocket||gauss||chaingun||bfg)&&kind==5)&&!((gauss||chaingun||bfg)&&kind==7)&&!((chaingun||bfg)&&kind==8)&&!(bfg&&kind==9)){check(!loaded[kind].valid);continue;}
            check(loaded[kind].valid
            &&loaded[kind].hashes[0]
            &&loaded[kind].assetPaths[0].find("LEFT.glb")!=std::string::npos
            &&((loaded[kind].hashes[1]&&loaded[kind].assetPaths[1].find("RIGHT.glb")!=std::string::npos)
                ||(!loaded[kind].hashes[1]&&loaded[kind].assetPaths[1]=="-")));
            if(pumps||workshop||plasma||rocket||gauss||chaingun||bfg||pistol)check(loaded[kind].metersPerAssetUnit>0&&loaded[kind].hasHudFrame
                &&loaded[kind].fistHashes[0]&&loaded[kind].fistHashes[1]);
        }
        check(!loaded[0].valid&&loaded[1].valid==pistol&&!loaded[11].valid);
    }
}
