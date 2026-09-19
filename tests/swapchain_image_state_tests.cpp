#include "../src/openxr/SwapchainImageState.h"
#include <cassert>

int main(){
    kharvox::SwapchainImageState image;
    assert(!image.owned());
    assert(image.acquired());
    assert(image.owned());
    assert(!image.releasable());
    assert(!image.released());
    assert(!image.waited(false));
    assert(image.owned());
    assert(!image.releasable());
    assert(image.waited(true));
    assert(image.releasable());
    assert(!image.acquired());
    assert(image.released());
    assert(!image.owned());
}
