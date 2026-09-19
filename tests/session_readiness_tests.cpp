#include "../src/openxr/SessionReadiness.h"
#include <cassert>

int main(){
    kharvox::SessionReadiness session;
    assert(!session.usable());
    session.created();
    assert(!session.usable());
    session.completed();
    assert(session.usable());
    session.reset();
    assert(!session.usable());
}
