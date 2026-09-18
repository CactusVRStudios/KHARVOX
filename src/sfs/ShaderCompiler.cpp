#include "ShaderCompiler.h"
#include "DoomLighting.h"
#include <spirv_glsl.hpp>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>

namespace kharvox::sfs {
namespace {
using namespace spirv_cross;
class StereoCompiler final:public CompilerGLSL {
    bool compute_{};
    bool computeStage_{};
    static bool promote(const SPIRType& type){return type.image.dim==spv::Dim2D&&!type.image.arrayed;}
    std::string eye()const{return computeStage_?(compute_?"khSfsEye":"0"):"gl_ViewIndex";}
    uint32_t coordinate(uint32_t source,bool integer,const std::string& layer){
        auto type=expression_type(source);type.vecsize=3;type.basetype=integer?SPIRType::Int:SPIRType::Float;
        const auto typeId=ir.increase_bound_by(2),id=typeId+1;
        set<SPIRType>(typeId,type);
        const auto text=(integer?"ivec3(ivec2(":"vec3(vec2(")+to_expression(source)+"), "+layer+")";
        set<SPIRExpression>(id,text,typeId,true);
        inherit_expression_dependencies(id,source);
        return id;
    }
    std::string sampledLayer(const TextureFunctionBaseArguments& args){
        // A comparison sampler addresses a light's shadow map, not scene
        // depth. The supplied DOOM profile explicitly keeps this sampling mono.
        if(args.imgtype->image.depth)return "0";
        auto image=convert_separate_image_to_expression(args.img);
        return "min(int("+eye()+"), textureSize("+image+(args.imgtype->image.ms?").z - 1)":", 0).z - 1)");
    }
protected:
    std::string image_type_glsl(const SPIRType& original,uint32_t id,bool member)override{
        auto type=original;if(promote(type))type.image.arrayed=true;
        return CompilerGLSL::image_type_glsl(type,id,member);
    }
    std::string to_function_name(const TextureFunctionNameArguments& original)override{
        if(!promote(*original.base.imgtype))return CompilerGLSL::to_function_name(original);
        if(original.base.is_proj)throw std::runtime_error("SFS: projective image sampling requires explicit conversion");
        auto args=original;auto type=*args.base.imgtype;type.image.arrayed=true;args.base.imgtype=&type;
        return CompilerGLSL::to_function_name(args);
    }
    std::string to_function_args(const TextureFunctionArguments& original,bool* forward)override{
        if(!promote(*original.base.imgtype))return CompilerGLSL::to_function_args(original,forward);
        auto args=original;auto type=*args.base.imgtype;
        args.coord=coordinate(args.coord,args.base.is_fetch,sampledLayer(args.base));
        args.coord_components=3;type.image.arrayed=true;args.base.imgtype=&type;
        return CompilerGLSL::to_function_args(args,forward);
    }
    void emit_instruction(const Instruction& instruction)override{
        const auto op=static_cast<spv::Op>(instruction.op);const auto* words=stream(instruction);
        // AMD DOOM modules use the legacy Groups vote opcode. SPIRV-Cross
        // otherwise emits an unimplemented comment and leaves its result ID
        // undefined. Only subgroup scope maps to GLSL's invocation vote.
        if(op==spv::OpGroupAll||op==spv::OpGroupAny){
            if(get_constant(words[2]).scalar()!=spv::ScopeSubgroup)
                throw std::runtime_error("SFS: legacy group vote requires subgroup scope");
            emit_unary_func_op(words[0],words[1],words[3],
                op==spv::OpGroupAll?"allInvocationsARB":"anyInvocationARB");
            require_extension_internal("GL_ARB_shader_group_vote");
            register_control_dependent_expression(words[1]);
            return;
        }
        // DOOM's particle module stores an integer-backed buffer flag into a
        // local bool. Its original driver accepts it; GLSL requires conversion.
        if(op==spv::OpStore&&expression_type(words[0]).basetype==SPIRType::Boolean&&
           (expression_type(words[1]).basetype==SPIRType::UInt||expression_type(words[1]).basetype==SPIRType::Int)){
            auto type=expression_type(words[1]);const bool unsignedValue=type.basetype==SPIRType::UInt;
            type.basetype=SPIRType::Boolean;
            if(type.vecsize!=1)throw std::runtime_error("SFS: unsupported vector boolean store");
            auto ids=ir.increase_bound_by(2);set<SPIRType>(ids,type);
            set<SPIRExpression>(ids+1,"("+to_expression(words[1])+(unsignedValue?" != 0u)":" != 0)"),ids,true);
            inherit_expression_dependencies(ids+1,words[1]);
            EmbeddedInstruction copy;copy.op=instruction.op;copy.count=instruction.count;copy.length=instruction.length;
            for(uint32_t j=0;j<instruction.length;++j)copy.ops.push_back(words[j]);copy.ops[1]=ids+1;
            CompilerGLSL::emit_instruction(copy);return;
        }
        const bool read=op==spv::OpImageRead,write=op==spv::OpImageWrite;
        if((read||write)&&promote(expression_type(words[write?0:2]))){
            EmbeddedInstruction copy;copy.op=instruction.op;copy.count=instruction.count;copy.length=instruction.length;
            for(uint32_t j=0;j<instruction.length;++j)copy.ops.push_back(words[j]);
            auto image=to_non_uniform_aware_expression(words[write?0:2]);
            auto index=write?1:3;
            copy.ops[index]=coordinate(words[index],true,"min(int("+eye()+"), imageSize("+image+").z - 1)");
            CompilerGLSL::emit_instruction(copy);return;
        }
        CompilerGLSL::emit_instruction(instruction);
        if((op==spv::OpImageQuerySize||op==spv::OpImageQuerySizeLod)&&promote(expression_type(words[2])))
            get<SPIRExpression>(words[1]).expression="("+get<SPIRExpression>(words[1]).expression+").xy";
    }
    std::string builtin_to_glsl(spv::BuiltIn builtin,spv::StorageClass storage)override{
        if(compute_){
            if(builtin==spv::BuiltInGlobalInvocationId)return "khSfsGlobalInvocationID";
            if(builtin==spv::BuiltInWorkgroupId)return "khSfsWorkGroupID";
            if(builtin==spv::BuiltInNumWorkgroups)return "khSfsNumWorkGroups";
        }
        return CompilerGLSL::builtin_to_glsl(builtin,storage);
    }
public:
    StereoCompiler(const std::vector<uint32_t>& words,bool compute):CompilerGLSL(words),compute_(compute),computeStage_(get_execution_model()==spv::ExecutionModelGLCompute){}
};
}

CompiledShader compileStereoShader(const std::vector<uint32_t>& original,const ShaderCompileOptions& request){
    if(request.indirectEye < -1 || request.indirectEye > 1 ||
       (request.indirectEye>=0&&(!request.computeStereo||request.profileReplacement)))
        throw std::runtime_error("SFS: unsupported indirect eye compilation policy");
    static std::once_flag initialization;
    std::call_once(initialization,[]{if(!glslang_initialize_process())throw std::runtime_error("glslang initialization failed");});
    StereoCompiler compiler(original,request.computeStereo);
    auto options=compiler.get_common_options();options.version=450;options.vulkan_semantics=true;
    compiler.set_common_options(options);
    const auto model=compiler.get_execution_model();
    if(model!=spv::ExecutionModelVertex&&model!=spv::ExecutionModelFragment&&model!=spv::ExecutionModelGLCompute)
        throw std::runtime_error("SFS: unsupported shader stage");
    if(request.computeStereo!=(model==spv::ExecutionModelGLCompute)&&request.computeStereo)
        throw std::runtime_error("SFS: compute transformation requested for graphics shader");
    auto resources=compiler.get_shader_resources();CompiledShader result;
    auto bindings=[&](const auto& list,bool storage){for(const auto& resource:list){const auto& type=compiler.get_type(resource.type_id);
        if(type.image.dim==spv::Dim2D&&!type.image.arrayed)result.arrayBindings.push_back({compiler.get_decoration(resource.id,spv::DecorationDescriptorSet),compiler.get_decoration(resource.id,spv::DecorationBinding),storage,type.image.depth});}};
    bindings(resources.sampled_images,false);bindings(resources.separate_images,false);bindings(resources.storage_images,true);
    auto source=compiler.compile();
    // The profile's virtual-material atlas pass writes atlas coordinates to
    // gl_Position and uses MVP only for a varying. A declared mvpmatrixw alone
    // must not shift the atlas. Recognize the same semantic form on other GPUs.
    const bool atlasPosition=source.find("vec2 atlasTilePos = vec2(in_VmtrTC.x, in_VmtrTC.y);")!=std::string::npos &&
        source.find("gl_Position = vec4((atlasTilePos * 2.0) - vec2(1.0), 0.0, 1.0);")!=std::string::npos;
    const bool vertexProjection=request.vertexProjection&&!atlasPosition&&!request.monoscopicView;
    result.vertexProjectionApplied=vertexProjection;
    // The 5d8a profile family also contains packed, virtual-textured world
    // geometry. Its near-camera depth pass must retain IPD even below the HUD
    // clip-W threshold, or depth and material coverage diverge underfoot.
    const bool packedWorld=source.find(".vertexxyzscale")!=std::string::npos&&
        source.find("in_VmtrTC")!=std::string::npos;
    result.screenSpaceUiApplied=vertexProjection&&request.screenSpaceUi&&!packedWorld;
    // Reconstruct the profile's horizontal clip correction in affine form.
    // stereo.z is KHARVOX's intercept. Also correct the two supplied fog variants
    // which subtract stereo.x rather than the convergence field in that term.
    static const std::regex displacement(R"(([A-Za-z_]\w*\.vk3d_params\[[^\]]+\]\.stereo)\.x \* \(([^()\n]+) - \1\.[xy]\))");
    source=std::regex_replace(source,displacement,"($1.x * ($2) + $1.z)");
    LightingCorrections lighting;
    if(!request.monoscopicView&&(model==spv::ExecutionModelFragment || (model==spv::ExecutionModelGLCompute &&
       (request.computeStereo || (request.profileReplacement && hasStereoStorageOutput(original))))))
        lighting=correctDoomLighting(source);
    result.clusterCorrections=lighting.clusters;result.worldCorrections=lighting.worldPositions;
    result.refractionCorrections=lighting.refractions;
    result.temporalCorrections=lighting.temporal;
    result.ssdoCorrections=lighting.ssdo;
    const auto versionEnd=source.find('\n');
    std::string prefix;
    if(model!=spv::ExecutionModelGLCompute)prefix="#extension GL_EXT_multiview : require\n";
    if(request.computeStereo)prefix+="uint khSfsEye;\nuvec3 khSfsGlobalInvocationID, khSfsWorkGroupID, khSfsNumWorkGroups;\n";
    if(vertexProjection||lighting.clusters||lighting.worldPositions||lighting.refractions||lighting.ssdo){
        for(const auto& u:resources.uniform_buffers)if(compiler.get_decoration(u.id,spv::DecorationDescriptorSet)==0&&compiler.get_decoration(u.id,spv::DecorationBinding)==31)
            throw std::runtime_error("SFS: projection descriptor binding collision");
        prefix+="layout(set=0,binding=31,std140) uniform KharvoxStereoProjection { layout(offset=64) mat4 clipFromCenter[2]; vec4 eyeTranslation[2]; mat4 previousClipFromCenter[2]; vec4 previousEyeTranslation[2]; } khSfsProjection;\n";
        if(lighting.clusters||lighting.worldPositions||lighting.refractions||lighting.ssdo)prefix+=lightingProjectionHelper(model!=spv::ExecutionModelGLCompute?"gl_ViewIndex":request.computeStereo?"khSfsEye":"gl_WorkGroupID.z / (gl_NumWorkGroups.z / 2u)");
    }
    if(vertexProjection){
        if(model!=spv::ExecutionModelVertex)throw std::runtime_error("SFS: projection requires a vertex shader");
        // Profile shaders use fixed-display separation/convergence here. The
        // headset transform replaces those position adjustments, retaining all
        // other shader-specific corrections and mono shadow replacements.
        size_t line=0;
        while(line<source.size()){
            auto end=source.find('\n',line);if(end==std::string::npos)end=source.size();
            const auto text=source.substr(line,end-line);
            if((text.find("gl_Position.x +=")!=std::string::npos||text.find("gl_Position.x -=")!=std::string::npos)
                &&text.find(".stereo.")!=std::string::npos)source.erase(line,end-line);
            else line=end;
            if(line<source.size())++line;
        }
    }
    // Put declarations after all #extension lines, which must precede declarations.
    source.insert(versionEnd+1,model!=spv::ExecutionModelGLCompute?"#extension GL_EXT_multiview : require\n":"");
    if(model!=spv::ExecutionModelGLCompute)prefix.erase(0,prefix.find('\n')+1);
    auto declaration=source.find("\n\n");if(declaration==std::string::npos)declaration=source.find('\n');
    source.insert(declaration+1,prefix);
    if(request.computeStereo||vertexProjection){
        const auto main=source.find("void main()");if(main==std::string::npos)throw std::runtime_error("SFS: missing GLSL entry point");
        source.replace(main,11,"void khSfsOriginalMain()");
        source+="\nvoid main() {\n";
        if(request.computeStereo){
            if(request.indirectEye>=0)source+="khSfsNumWorkGroups = gl_NumWorkGroups;\nkhSfsEye = "+std::to_string(request.indirectEye)+"u;\nkhSfsWorkGroupID = gl_WorkGroupID;\nkhSfsGlobalInvocationID = gl_GlobalInvocationID;\n";
            else source+="khSfsNumWorkGroups = gl_NumWorkGroups; khSfsNumWorkGroups.z /= 2u;\nkhSfsEye = gl_WorkGroupID.z / khSfsNumWorkGroups.z;\nkhSfsWorkGroupID = gl_WorkGroupID; khSfsWorkGroupID.z %= khSfsNumWorkGroups.z;\nkhSfsGlobalInvocationID = gl_GlobalInvocationID; khSfsGlobalInvocationID.z -= khSfsEye * khSfsNumWorkGroups.z * gl_WorkGroupSize.z;\n";
        }
        source+="khSfsOriginalMain();\n";
        if(vertexProjection){
            // Screen UI has no metric depth for IPD, but still needs the HMD's
            // asymmetric FOV transform. Equal clip coordinates in both eyes
            // are different viewing rays and cause binocular double images.
            source+="gl_Position = khSfsProjection.clipFromCenter[gl_ViewIndex] * gl_Position;\n";
            if(result.screenSpaceUiApplied)source+="if (gl_Position.w > 8.0) ";
            source+="gl_Position += khSfsProjection.eyeTranslation[gl_ViewIndex];\n";
        }
        source+="}\n";
    }
    result.glsl=source;
    glslang_input_t input{};input.language=GLSLANG_SOURCE_GLSL;
    input.stage=model==spv::ExecutionModelVertex?GLSLANG_STAGE_VERTEX:model==spv::ExecutionModelFragment?GLSLANG_STAGE_FRAGMENT:GLSLANG_STAGE_COMPUTE;
    input.client=GLSLANG_CLIENT_VULKAN;input.client_version=GLSLANG_TARGET_VULKAN_1_1;
    input.target_language=GLSLANG_TARGET_SPV;input.target_language_version=GLSLANG_TARGET_SPV_1_3;
    input.code=source.c_str();input.default_version=450;input.default_profile=GLSLANG_NO_PROFILE;
    input.messages=static_cast<glslang_messages_t>(GLSLANG_MSG_SPV_RULES_BIT|GLSLANG_MSG_VULKAN_RULES_BIT);input.resource=glslang_default_resource();
    std::unique_ptr<glslang_shader_t,decltype(&glslang_shader_delete)> shader(glslang_shader_create(&input),glslang_shader_delete);
    if(!shader)throw std::runtime_error("SFS: shader allocation failed");
    if(!glslang_shader_preprocess(shader.get(),&input)||!glslang_shader_parse(shader.get(),&input))
        throw std::runtime_error(std::string("SFS shader compile: ")+glslang_shader_get_info_log(shader.get())+"\n"+source);
    std::unique_ptr<glslang_program_t,decltype(&glslang_program_delete)> program(glslang_program_create(),glslang_program_delete);
    if(!program)throw std::runtime_error("SFS: program allocation failed");
    glslang_program_add_shader(program.get(),shader.get());
    if(!glslang_program_link(program.get(),input.messages))throw std::runtime_error(glslang_program_get_info_log(program.get()));
    glslang_program_SPIRV_generate(program.get(),input.stage);
    const auto size=glslang_program_SPIRV_get_size(program.get());if(!size)throw std::runtime_error("SFS: SPIR-V generation failed");
    result.words.resize(size);glslang_program_SPIRV_get(program.get(),result.words.data());
    return result;
}
bool hasStereoStorageOutput(const std::vector<uint32_t>& original){
    spirv_cross::Compiler compiler(original);
    if(compiler.get_execution_model()!=spv::ExecutionModelGLCompute)return false;
    for(const auto& image:compiler.get_shader_resources().storage_images)
        if(compiler.get_type(image.type_id).image.dim==spv::Dim2D&&!compiler.has_decoration(image.id,spv::DecorationNonWritable))return true;
    return false;
}
bool needsHeadsetProjection(const std::vector<uint32_t>& words,bool profileReplacement,bool shadowPass){
    // A TV profile may intentionally remove separation from UI/mask geometry.
    // No active stereo UBO does not imply that its centered MVP already uses
    // the headset FOV. Keep explicit stereo exceptions in biased passes, but
    // never apply this fallback to an unshifted shared shadow-map shader.
    if(profileReplacement&&needsStereoProjection(words,true))return true;
    return !shadowPass&&needsStereoProjection(words,false);
}
bool needsStereoProjection(const std::vector<uint32_t>& original,bool profileReplacement){
    spirv_cross::Compiler compiler(original);
    if(compiler.get_execution_model()!=spv::ExecutionModelVertex)return false;
    for(const auto& buffer:compiler.get_shader_resources().uniform_buffers){
        if(profileReplacement){if(compiler.get_decoration(buffer.id,spv::DecorationDescriptorSet)==0&&compiler.get_decoration(buffer.id,spv::DecorationBinding)==30&&!compiler.get_active_buffer_ranges(buffer.id).empty())return true;}
        else {
            const auto& type=compiler.get_type(buffer.base_type_id);
            bool view=false,projection=false,viewProjection=false;
            const auto active=compiler.get_active_buffer_ranges(buffer.id);
            for(uint32_t i=0;i<type.member_types.size();++i){
                const auto name=compiler.get_member_name(buffer.base_type_id,i);
                bool used=false;for(const auto& range:active)used|=range.index==i;
                if(!used)continue;
                if(name=="mvpmatrixw")return true;
                view|=name=="viewmatrixw";
                projection|=name=="projectionmatrixw";
                viewProjection|=name=="viewprojectionmatrixw";
            }
            // GPU particles assemble view and projection separately; distant
            // world geometry can use VP directly. Both still need per-eye clip.
            if(viewProjection||(view&&projection))return true;
        }
    }
    return false;
}
}
