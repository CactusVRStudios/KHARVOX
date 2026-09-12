#pragma once
namespace kharvox::native {
// A full scene-depth CLEAR is its own initializer. A seed in the last root
// command buffer may execute AFTER that producer in an earlier command buffer.
inline bool sceneDepthClearsItself(bool sceneD24S8, bool fullImage,
                                  bool clearsDepth, bool clearsStencil) {
    return sceneD24S8 && fullImage && clearsDepth && clearsStencil;
}
}
