#include "../src/openxr/CommandRecordingState.h"
#include <cassert>

int main(){
    kharvox::CommandRecordingState resetFailure;
    assert(!resetFailure.reset(false));
    assert(!resetFailure.executable());

    kharvox::CommandRecordingState beginFailure;
    assert(beginFailure.reset(true));
    assert(!beginFailure.begun(false));
    assert(!beginFailure.executable());

    kharvox::CommandRecordingState endFailure;
    assert(endFailure.reset(true));
    assert(endFailure.begun(true));
    assert(!endFailure.ended(false));
    assert(!endFailure.executable());

    kharvox::CommandRecordingState complete;
    assert(complete.reset(true));
    assert(complete.begun(true));
    assert(complete.ended(true));
    assert(complete.executable());
}
