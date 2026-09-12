#pragma once
#include <cstdint>
namespace kharvox::native {
// Native draws world, weapon and custom hands from each eye's camera. The
// donor's centered-view weapon experiment must not replace either eye's view,
// even when its legacy switch is enabled. Use for both publish and consume.
constexpr bool allowLegacyGunUnshift(bool nativeBackend,bool enabled,bool symmetric){
 return !nativeBackend&&enabled&&symmetric;
}
enum class Phase { Off, Observing, Warming, Ready, RestartAer };
constexpr bool canTransition(Phase from,Phase to){return from!=Phase::RestartAer&&(from==to||to==Phase::RestartAer||(from==Phase::Off&&to==Phase::Observing)||(from==Phase::Observing&&to==Phase::Warming)||(from==Phase::Warming&&to==Phase::Ready)||(from==Phase::Ready&&to==Phase::Warming));}
struct EyeWorkPlan {bool renderRight;bool prepareStereo;bool prepareMirrors;bool captureReplay;bool captureInputPlans;};
constexpr EyeWorkPlan eyeWorkPlan(bool leftOnly,bool retainPreparation,bool inputsOnly=false,bool noReplayCapture=false,bool noInputPlans=false){
 return {!leftOnly,!leftOnly||retainPreparation,!leftOnly||(retainPreparation&&!inputsOnly),
         !leftOnly||(retainPreparation&&!(inputsOnly&&noReplayCapture)),
         !leftOnly||(retainPreparation&&!(inputsOnly&&(noReplayCapture||noInputPlans)))};
}
enum class Backend { Aer, Native };
// The old AFW request is deliberately ignored during configuration migration.
constexpr Backend selectBackend(bool native, bool /*obsoleteAfw*/, bool restartAer){return restartAer?Backend::Aer:native?Backend::Native:Backend::Aer;}
constexpr bool allowFsr(Backend b){return b==Backend::Aer||b==Backend::Native;}
struct PairIdentity {uint64_t leftImage{},rightImage{},leftFrame{},rightFrame{},rootFrame{},generation{},currentGeneration{};uint32_t leftEye{},rightEye{};bool finalPass{},initialized{},layoutsKnown{};};
constexpr bool validPair(const PairIdentity& p){return p.leftImage&&p.rightImage&&p.leftImage!=p.rightImage&&p.leftFrame&&p.leftFrame==p.rightFrame&&p.leftFrame==p.rootFrame&&p.generation==p.currentGeneration&&p.leftEye==0&&p.rightEye==1&&p.finalPass&&p.initialized&&p.layoutsKnown;}
// Separate acceptance path: never weaken the two-image stereo contract.
constexpr bool validLeftDiagnostic(bool enabled,uint64_t left,uint64_t finalTarget,uint64_t frame,uint64_t root,uint64_t final,bool submitted,bool recordingClosed){return enabled&&left&&left==finalTarget&&frame&&frame==root&&frame==final&&submitted&&recordingClosed;}
// A successful submit consumes binary waits even if a subsequent fence wait fails.
struct PresentWaitOwnership {bool consumed{};bool submit(bool success){if(consumed)return false;if(success)consumed=true;return success;}bool downstreamMustWait()const{return !consumed;}};
constexpr bool canRetire(bool submitted,bool completionSucceeded,bool recordingOutstanding){return submitted&&completionSucceeded&&!recordingOutstanding;}
constexpr bool canSeedStorage(bool recording,bool insidePass,bool submitted){return recording&&!insidePass&&!submitted;}
constexpr bool canReadMirror(bool allocated,bool initialized,bool layoutKnown){return allocated&&initialized&&layoutKnown;}
// Converged allocation alone is insufficient: publishing a frame seeded from
// the completed first eye would feed its tonemapped color into the second eye.
constexpr bool canCompleteFrame(bool finalImage,bool root,bool converged,bool originalInputsPreserved){return finalImage&&root&&converged&&originalInputsPreserved;}
}
