#include "../src/native/NativeDescriptorLifetime.h"
#include <cstdlib>
#include <iostream>
using namespace kharvox::native;
static void require(bool ok){if(!ok)std::abort();}
int main(){
 DescriptorPoolLedger ledger;ledger.allocated(1,10);ledger.allocated(1,11);ledger.allocated(2,20);
 ledger.freed(10);require(ledger.size()==2);require(ledger.reset(1)==std::vector<uint64_t>{11});require(ledger.size()==1);
 ledger.allocated(3,20);require(ledger.reset(2).empty());require(ledger.reset(3)==std::vector<uint64_t>{20});require(!ledger.size());
 // Repeated pool churn must not retain old source-set identities.
 for(uint64_t frame=0;frame<10000;++frame){for(uint64_t i=0;i<200;++i)ledger.allocated(1,frame*200+i);require(ledger.size()==200);require(ledger.reset(1).size()==200);require(!ledger.size());}
 require(ledger.reset(99).empty());ledger.freed(999);
 std::cout<<"Descriptor pool reset/free/recycling and bounded churn passed\n";
}
