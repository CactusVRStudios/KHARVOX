// Read-only compatibility audit against the user's locally compiled profile.
// Usage: KharvoxSfsProfileAudit <capture directory> <compiled profile directory>
#include "../src/sfs/ShaderProfile.h"
#include <iostream>
#include <set>
#include <sstream>
int main(int argc,char** argv){try{
    if(argc!=3){std::cerr<<"Expected capture and compiled profile directories\n";return 2;}
    const std::filesystem::path capture=argv[1],profile=argv[2];
    std::ifstream records(capture/"pipeline-variants.tsv");
    if(!records)throw std::runtime_error("Pipeline capture missing");
    std::set<std::filesystem::path> resolved;
    std::string primary,variant;uint32_t stage;size_t total=0;
    while(records>>primary>>variant>>stage){
        auto selected=kharvox::sfs::loadProfileShader(profile,std::stoull(primary,nullptr,16),std::stoull(variant,nullptr,16),static_cast<VkShaderStageFlagBits>(stage));
        if(selected)resolved.insert(selected.path.filename());
        ++total;
    }
    size_t variants=0,matched=0;
    for(const auto& item:std::filesystem::directory_iterator(profile)){
        const auto name=item.path().filename().string();
        if(item.path().extension()!=".spv")continue;
        const auto first=name.find('_');if(first==std::string::npos)continue;
        const auto second=name.find('_',first+1);if(second==std::string::npos)continue;
        ++variants;if(resolved.count(item.path().filename()))++matched;
        else std::cout<<"Not observed: "<<name<<'\n';
    }
    std::cout<<total<<" stage records; "<<resolved.size()<<" compiled replacements resolved; "<<matched<<'/'<<variants<<" exact profile variants observed\n";
    return variants&&matched==variants?0:1;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
