#include "../src/hud/TutorialRenderPolicy.h"
#include "../src/hud/TutorialBindingText.h"
#include <cstdlib>
#include <limits>
int main(){
 auto check=[](bool v){if(!v)std::abort();};
 check(kharvox::replaceTutorialBindings("Press ^g_attack2^7",false,false)=="Press ^g[R Stick click]^7");
 check(kharvox::replaceTutorialBindings("_quick3 / _jump",true,true)=="[L Stick up] / [Y]");
 check(kharvox::replaceTutorialBindings("_quick3 / _attack2",true,false)=="[R Stick up] / [L Stick click]");
 check(kharvox::replaceTutorialBindings("_menuCancel _quick30 foo_quick3",false,false)=="_menuCancel _quick30 foo_quick3");
 check(kharvox::replaceTutorialBindings("Druecke ^g_quick2^7",false,false)=="Druecke ^g[L Grip tap (outside grab range)]^7");
 std::array<float,6> m{1,1,0,0,0,0};
 auto voice=kharvox::voiceCommMatrix(m,m,1920,1080);
 check(std::abs(voice[0]-.54f)<.001f&&std::abs(voice[4]-1056)<.001f&&std::abs(voice[5]-777.6f)<.001f);
 // Canvas rotation must rotate the rightward offset, not shift in screen X.
 std::array<float,6> rotated{0,0,-2,2,12,20};
 auto vr=kharvox::voiceCommMatrix(rotated,rotated,1920,1080);
 check(std::abs(vr[4]+1543.2f)<.001f&&std::abs(vr[5]-2132)<.001f);
 check(kharvox::voiceCommMatrix(m,m,0,1080)==m);
 check(kharvox::voiceCommMatrix(m,m,1920,0)==m);
 std::array<float,6> boss{.8f,.8f,0,0,100,108};
 auto lowered=kharvox::bossHealthBarMatrix(boss,m,1920,1080);
 check(std::abs(lowered[4]-616)<.001f&&lowered[5]==378);
 check(std::abs((lowered[4]+lowered[0]*1075)-960)<.001f);
 for(int i=0;i<4;++i)check(std::abs(lowered[i]-.4f*boss[i])<.001f);
 auto twice=boss;for(float& v:twice)v*=2;
 std::array<float,6> twiceRoot{2,2,0,0,0,0};
 auto doubledBoss=kharvox::bossHealthBarMatrix(twice,twiceRoot,1920,1080);
 for(int i=0;i<6;++i)check(doubledBoss[i]==2*lowered[i]);
 check(kharvox::bossHealthBarMatrix(boss,m,1920,0)==boss);
 check(kharvox::bossHealthBarMatrix(boss,m,1920,std::numeric_limits<float>::infinity())==boss);
 auto rune=kharvox::runeCounterMatrix(m,m,1920,1080);
 check(std::abs(rune[0]-.425f)<.001f);
 // Native centered top timer stays centered and moves below the 25% cut line.
 check(std::abs((rune[4]+rune[0]*960)-960)<.001f);
 check((rune[5]+rune[1]*108)/1080>.30f);
 std::array<float,6> child{1,1,0,0,960,108};
 auto rc=kharvox::runeCounterMatrix(child,m,1920,1080);
 check(std::abs(rc[4]-960)<.001f);
 check(kharvox::runeCounterMatrix(m,m,1920,0)==m);
 auto kill=kharvox::runeCounterMatrix(child,m,1920,1080,true);
 check(std::abs(kill[5]-rc[5]-43.2f)<.001f);
 check(std::abs(rc[5]/1080-.36f)<.001f);
 // Changing render pixels and viewport translation must preserve relative placement.
 std::array<float,6> root2{2,2,0,0,12,20};
 std::array<float,6> child2{2,2,0,0,1932,236};
 auto rc2=kharvox::runeCounterMatrix(child2,root2,1920,1080,true);
 check(std::abs(rc2[4]-(kill[4]*2+12))<.001f);
 check(std::abs(rc2[5]-(kill[5]*2+20))<.001f);
 auto mission=kharvox::missionTextMatrix(m,1920,1080);
 check(std::abs(mission[4]-384.0f)<.001f && std::abs(mission[5]-194.4f)<.001f);
 for(int i=0;i<4;++i)check(mission[i]==m[i]);
 auto r=kharvox::tutorialTextMatrix(m,1920,1080);
 check(std::abs(r[0]-.55f)<.0001f);
 check(std::abs(r[4]+r[0]*960-960)<.001f);
 check(std::abs((r[5]+r[1]*972)/1080-.62f)<.001f);
 std::array<float,6> doubled{2,2,0,0,12,20};
 auto d=kharvox::tutorialTextMatrix(doubled,1920,1080);
 check(std::abs(d[4]-(r[4]*2+12))<.001f);
 check(std::abs(d[5]-(r[5]*2+20))<.001f);
 check(kharvox::tutorialTextMatrix(m,0,1080)==m);
 check(kharvox::tutorialTextMatrix(m,1920,std::numeric_limits<float>::infinity())==m);
}
