#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include "../src/hands/HandInteractionPolicy.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
bool nodeInScene(const cgltf_node* node, const cgltf_scene* scene) {
    if (!scene) return true;
    const cgltf_node* root = node;
    while (root && root->parent) root = root->parent;
    for (cgltf_size index = 0; index < scene->nodes_count; ++index)
        if (scene->nodes[index] == root) return true;
    return false;
}

bool validate(const std::filesystem::path& path,
    const char* expectedSceneName, std::size_t* vertices=nullptr) {
    cgltf_options options{};
    cgltf_data* data{};
    const auto name = path.u8string();
    if (cgltf_parse_file(&options, name.c_str(), &data)
            != cgltf_result_success)
        return false;
    const bool buffersLoaded = cgltf_load_buffers(&options, data, name.c_str())
        == cgltf_result_success;
    const cgltf_node* meshNode{};
    const cgltf_primitive* primitive{};
    if (buffersLoaded && data->scene) {
        for (cgltf_size index = 0; index < data->nodes_count; ++index) {
            const auto& node = data->nodes[index];
            if (node.mesh && node.mesh->primitives_count
                    && nodeInScene(&node, data->scene)) {
                meshNode = &node;
                primitive = &node.mesh->primitives[0];
                break;
            }
        }
    }
    bool position{}, normal{}, uv{}, joints{}, weights{};
    if (primitive) {
        for (cgltf_size index = 0; index < primitive->attributes_count;
                ++index) {
            const auto& attribute = primitive->attributes[index];
            position |= attribute.type == cgltf_attribute_type_position;
            if(vertices&&attribute.type==cgltf_attribute_type_position)*vertices=attribute.data->count;
            normal |= attribute.type == cgltf_attribute_type_normal;
            uv |= attribute.type == cgltf_attribute_type_texcoord
                && attribute.index == 0;
            joints |= attribute.type == cgltf_attribute_type_joints
                && attribute.index == 0;
            weights |= attribute.type == cgltf_attribute_type_weights
                && attribute.index == 0;
        }
    }
    const bool staticPose = meshNode && !meshNode->skin && !joints && !weights;
    const bool bakedRigSource = meshNode && meshNode->skin && joints && weights
        && meshNode->skin->inverse_bind_matrices
        && meshNode->skin->joints_count > 0;
    const bool valid = buffersLoaded && data->scene && data->scene->name
        && std::string(data->scene->name) == expectedSceneName
        && meshNode && (staticPose || bakedRigSource)
        && primitive && primitive->type == cgltf_primitive_type_triangles
        && primitive->indices && primitive->material
        && primitive->material->has_pbr_metallic_roughness
        && primitive->material->pbr_metallic_roughness
            .base_color_texture.texture
        && position && normal && uv;
    cgltf_free(data);
    return valid;
}

std::vector<std::uint8_t> read(const std::filesystem::path&path){
    std::ifstream in(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),{}};
}
bool validateFingerData(const std::filesystem::path&assets,const char*hand,std::size_t vertices){
    using namespace kharvox::hands;
    const std::string prefix=std::string("DOOM_")+hand+"_HAND_";
    const auto fist=read(assets/(prefix+"FIST.glb")),gun=read(assets/(prefix+"GUN.glb"));
    const auto morphs=read(assets/(prefix+"FINGERS.bin"));
    FingerMorphHeader header{};
    if(fist.empty()||gun.empty()||!readFingerMorphHeader(morphs.data(),morphs.size(),vertices,
        handAssetHash(fist.data(),fist.size()),header)||header.gunHash!=handAssetHash(gun.data(),gun.size()))return false;
    for(std::size_t shape=0;shape<3;++shape){
        float maximum{};std::size_t moving{};
        for(std::size_t i=0;i<vertices;++i){
            float values[6]{};std::memcpy(values,morphs.data()+fingerMorphHeaderBytes+(shape*vertices+i)*24,24);
            for(float value:values)if(!std::isfinite(value)||std::abs(value)>10.f)return false;
            const float distance=std::sqrt(values[0]*values[0]+values[1]*values[1]+values[2]*values[2]);
            maximum=std::max(maximum,distance);if(distance>.001f)++moving;
        }
        if(maximum<.02f||maximum>1.5f||moving<25)return false;
    }
    return true;
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const std::filesystem::path assets(argv[1]);
    std::size_t leftVertices{},rightVertices{};
    if (!validate(assets / "DOOM_LEFT_HAND_FIST.glb", "L_Pose_1", &leftVertices)) {
        std::cerr << "left fist GLB does not match the baked-pose contract\n";
        return 2;
    }
    if (!validate(assets / "DOOM_RIGHT_HAND_FIST.glb", "R_Pose_1", &rightVertices)) {
        std::cerr << "right fist GLB does not match the baked-pose contract\n";
        return 3;
    }
    if (!validate(assets / "DOOM_LEFT_HAND_GUN.glb", "L_Pose_2")) {
        std::cerr << "left gun GLB does not match the baked-pose contract\n";
        return 4;
    }
    if (!validate(assets / "DOOM_RIGHT_HAND_GUN.glb", "R_Pose_2")) {
        std::cerr << "right gun GLB does not match the baked-pose contract\n";
        return 5;
    }
    if(!validateFingerData(assets,"LEFT",leftVertices)||!validateFingerData(assets,"RIGHT",rightVertices)){
        std::cerr<<"finger morphs/wrist frames do not match the authored GLBs\n";return 6;
    }
    return 0;
}
