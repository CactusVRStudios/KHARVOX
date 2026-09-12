#include "../src/native/NativeInlineStartup.h"
#include <iostream>
using namespace kharvox::native;
int main(){
 int errors=0;auto check=[&](bool value){if(!value)++errors;};
 int value=2,writes=0;bool readable=true,allowed=true,apply=true;
 auto read=[&](int& out){out=value;return readable;};
 auto write=[&](const char* text){++writes;check(text[0]=='1'&&text[1]==0);if(allowed&&apply)value=1;return allowed;};
 InlineStartup startup;
 check(startup.initialize(true,read,write)==InlineStartupResult::Applied);
 check(startup.before==2&&startup.after==1&&writes==1);
 value=2;check(startup.initialize(true,read,write)==InlineStartupResult::UnsafeBoundary);
 check(writes==1&&value==2); // no gameplay reassertion after an external change
 InlineStartup late;check(late.initialize(false,read,write)==InlineStartupResult::UnsafeBoundary);
 check(writes==1); // cannot change mode after rendering starts
 value=1;InlineStartup existing;check(existing.initialize(true,read,write)==InlineStartupResult::AlreadyInline);
 check(writes==1); // preserve a correctly initialized value
 value=0;InlineStartup unsupported;check(unsupported.initialize(true,read,write)==InlineStartupResult::UnsupportedValue);
 check(writes==1); // no unknown-mode conversion
 value=2;allowed=false;InlineStartup denied;check(denied.initialize(true,read,write)==InlineStartupResult::WriteFailed);
 check(value==2);allowed=true;apply=false;
 InlineStartup ignored;check(ignored.initialize(true,read,write)==InlineStartupResult::WriteFailed);
 readable=false;const auto previous=writes;InlineStartup unreadable;
 check(unreadable.initialize(true,read,write)==InlineStartupResult::ReadFailed&&writes==previous);
 readable=true;apply=true;InlineStartup lostRead;
 check(lostRead.initialize(true,read,[&](const char* text){auto result=write(text);readable=false;return result;})==InlineStartupResult::ReadFailed);
 std::cout<<"Native inline startup failures="<<errors<<'\n';return errors?1:0;
}
