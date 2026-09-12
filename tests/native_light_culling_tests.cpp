#include "../src/native/NativeLightCullingPolicy.h"
#include <cstdlib>
#include <iostream>
using namespace kharvox::native;
static void require(bool ok){if(!ok)std::abort();}
int main(){
 LightCullingScope scope;
 LightCullingValue engine{"0",0};int reads{},writes{};bool deny{},readable=true;
 auto read=[&](LightCullingValue& into){++reads;if(!readable)return false;into=engine;return true;};
 auto write=[&](const char* text){++writes;if(!deny)engine={text,std::atoi(text)};};
 require(scope.update(false,read,write)&&reads==0&&writes==0); // AER/menu never acquires
 require(scope.update(true,read,write)&&scope.held&&engine.integer==1);
 const int firstWrites=writes;
 require(scope.update(true,read,write)&&writes==firstWrites); // no repeated setter work
 require(scope.update(false,read,write)&&!scope.held&&engine.text=="0"); // Cine/menu restore
 require(scope.update(true,read,write)&&engine.integer==1); // gameplay resumes
 engine={"2",2}; // external change after our frame: preserve on exit
 require(scope.update(false,read,write)&&engine.integer==2);
 require(scope.update(true,read,write)&&engine.integer==1);
 require(scope.update(false,read,write)&&engine.text=="2"); // owned restoration text
 engine={"1",1};require(scope.update(true,read,write));
 require(scope.update(false,read,write)&&engine.integer==1); // pre-existing user value
 engine={"0",0};deny=true;
 require(!scope.update(true,read,write)); // denied activation cannot appear successful
 deny=false;require(scope.update(true,read,write)&&engine.integer==1);
 deny=true;require(!scope.update(false,read,write)&&scope.held); // denied restoration -> failure
 deny=false;require(scope.update(false,read,write)&&!scope.held);
 readable=false;require(!scope.update(true,read,write)); // unknown identity/read fails closed
 std::cout<<"Native light culling scope, restoration and failure checks passed\n";
}
