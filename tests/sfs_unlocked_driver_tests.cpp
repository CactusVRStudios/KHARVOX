#include "../src/sfs/UnlockedDriverScope.h"
#include <future>
#include <shared_mutex>
#include <stdexcept>
int main(){
 std::shared_mutex mutex;std::unique_lock lock(mutex);
 for(bool fail:{false,true}){
  try{
   kharvox::sfs::UnlockedDriverScope unlocked(lock);
   if(lock.owns_lock())return 1;
   if(!std::async(std::launch::async,[&]{if(!mutex.try_lock_shared())return false;mutex.unlock_shared();return true;}).get())return 2;
   if(fail)throw std::runtime_error("driver failure");
  }catch(const std::runtime_error&){}
  if(!lock.owns_lock())return 3;
 }
}
