#include "../src/sfs/ShaderCompiler.h"
#include <filesystem>
#include <fstream>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc<3){std::cerr<<"Usage: KharvoxSfsCompile source.spv output.spv [projection]\n";return 2;}
    const auto size=std::filesystem::file_size(argv[1]);if(size<20||size%4||size>16*1024*1024)throw std::runtime_error("Invalid input size");
    std::vector<uint32_t> words(size/4);std::ifstream input(argv[1],std::ios::binary);
    if(!input.read(reinterpret_cast<char*>(words.data()),size))throw std::runtime_error("Read failed");
    kharvox::sfs::ShaderCompileOptions options;options.vertexProjection=kharvox::sfs::needsStereoProjection(words,argc>3&&std::string(argv[3])=="profile")||(argc>3&&std::string(argv[3])=="projection");
    options.computeStereo=kharvox::sfs::hasStereoStorageOutput(words);
    if(argc>3&&std::string(argv[3])=="profile")options.computeStereo=false;
    auto result=kharvox::sfs::compileStereoShader(words,options);
    std::ofstream output(argv[2],std::ios::binary);output.write(reinterpret_cast<const char*>(result.words.data()),result.words.size()*4);
    if(!output)throw std::runtime_error("Write failed");
    std::ofstream source(std::string(argv[2])+".glsl");source<<result.glsl;
    std::cout<<result.arrayBindings.size()<<" promoted descriptor bindings\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
