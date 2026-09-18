// AER-only, validated pre-cull producer. No Native replay hooks are installed.
using AerBuildViewMatricesFn=void(__fastcall*)(void*,void*,uint64_t);
using AerModelMatrixFn=void(__fastcall*)(void*,const void*,void*,float,float,float,bool);
static AerModelMatrixFn realAerModelMatrix{};
static kharvox::AerWeaponAttachments aerWeaponAttachments;
static bool validateAerModelMatrix(unsigned char* base){
 DWORD64 image{};const auto address=reinterpret_cast<DWORD64>(base+0x1ade630);
 const auto entry=RtlLookupFunctionEntry(address,&image,nullptr);
 if(!entry||image+entry->BeginAddress!=address||entry->EndAddress-entry->BeginAddress!=0x1d3
    ||((base[entry->UnwindData]>>3)&UNW_FLAG_CHAININFO))return false;
 uint64_t hash=14695981039346656037ull;
 for(size_t i=0;i<0x707;++i)hash=(hash^base[0x1ade630+i])*1099511628211ull;
 return hash==0x878fd78f095fbc60ull;
}
static void __fastcall hookAerModelMatrix(void* output,const void* rawModel,void* rawView,
    float x,float y,float z,bool jitter){
 const auto model=reinterpret_cast<uintptr_t>(rawModel),view=reinterpret_cast<uintptr_t>(rawView);
 const void* selected=rawModel;void* selectedView=rawView;
 alignas(16) std::array<unsigned char,0xb64> copy;
 alignas(16) std::array<unsigned char,0x11b0> viewCopy;
 float pose[12]{},target[12]{},viewPose[14]{},scale[3]{},matrix[16]{};
 unsigned char flags{},billboard{};uint64_t matched{};kharvox::AerWorldView camera;
 int status=0;float matrixError=0,cameraError=0;
 bool bindingRecovered=false;uintptr_t asset{};
 // Tracked weapons and independently observed rigid GUI attachments only.
 // Bit 0x10 uses a separate camera at view+0x10B8, not world V+0x11F0.
 if(model&&view&&safe_read_bytes(model+0xba,&flags,1)
    &&safe_read_bytes(model+0xb9,&billboard,1)&&kharvox::aerDrawSourceEligible(flags,billboard)
    &&((flags&0x20)||((flags&~8u)==0&&(billboard==0||billboard==8)))
    &&safe_read_bytes(model+0x30,pose,sizeof(pose))
    &&safe_read_bytes(view+0x940,viewPose,12*sizeof(float))
    &&safe_read_bytes(view+0x8f0,viewPose+12,2*sizeof(float))
    &&((flags&0x20)||([&]{float d=0;for(int i=0;i<3;++i)d+=(pose[i]-viewPose[i])*(pose[i]-viewPose[i]);return d<100.f*100.f;})())
    &&KharvoxCameraRecognizeAerWorldView(viewPose,viewPose+12,camera)==kharvox::AerWorldViewResult::Unchanged){
    safe_read_bytes(model,&asset,sizeof(asset));
    const kharvox::AerSourceKey key{camera.poseId,camera.level,camera.eye,camera.domain};
    const auto present=KharvoxCameraCurrentPresentSerial();
    if(flags&0x20){
        status=KharvoxWeaponResolveAerDraw(key,pose,pose+3,target,target+3,matched,model,asset,&bindingRecovered,viewPose);
        if(status==1||status==2||status==6)aerWeaponAttachments.parent(model,asset,key,present,pose,target,viewPose);
    }else if((flags&~8u)==0&&(billboard==0||billboard==8)){
        float distance2=0;for(int i=0;i<3;++i)distance2+=(pose[i]-viewPose[i])*(pose[i]-viewPose[i]);
        if(distance2<100.f*100.f&&aerWeaponAttachments.resolve(model,asset,key,present,pose,target))status=7;
    }
    if((status==1||status==2||status==6||status==7)&&safe_read_bytes(model+0x60,scale,sizeof(scale))
        &&kharvox::aerDrawModelMatrix(target,target+3,scale,matrix)
        &&safe_read_bytes(model,copy.data(),copy.size())){
        float previous[16]{};std::memcpy(previous,copy.data()+0xb24,sizeof(previous));
        for(int i=0;i<16;++i)matrixError=std::max(matrixError,std::abs(previous[i]-matrix[i]));
        if(status==2||status==6||status==7||matrixError>.00001f){
            std::memcpy(copy.data()+0x30,target,sizeof(target));
            std::memcpy(copy.data()+0xb24,matrix,sizeof(matrix));
            selected=copy.data();if(status!=6&&status!=7)status=2;
        }
        if(flags&0x10){
            // This fingerprinted branch reads the auxiliary origin/axis and
            // P+0x1170. Give this one tracked draw its own source-camera copy;
            // keep its existing projection instead of changing global state.
            if(safe_read_bytes(view,viewCopy.data(),viewCopy.size())){
                float previousCamera[12]{};std::memcpy(previousCamera,viewCopy.data()+0x10b8,sizeof(previousCamera));
                for(int i=0;i<12;++i)cameraError=std::max(cameraError,std::abs(previousCamera[i]-viewPose[i]));
                if(cameraError>.00001f){
                    std::memcpy(viewCopy.data()+0x10b8,viewPose,sizeof(previousCamera));
                    selectedView=viewCopy.data();if(status!=6)status=2;
                }
            }else{selected=rawModel;status=5;}
        }
    }else if(status==1||status==2||status==6||status==7)status=5;
    if(status&&kharvox::pose_trace::active.load(std::memory_order_relaxed)){
        kharvox::pose_trace::Event e{};e.kind=kharvox::pose_trace::WeaponDrawModel;e.source=model;e.frame=KharvoxCameraCurrentPresentSerial();
        e.poseId=camera.poseId;e.eye=camera.eye;e.revision=matched;e.status=status;
        std::memcpy(e.data,pose,sizeof(pose));e.data[12]=matrixError;e.data[13]=cameraError;e.flags=flags|(bindingRecovered?0x10000u:0u);kharvox::pose_trace::record(e);
    }
    static std::array<std::atomic<uint64_t>,8> counts{};
    const auto n=counts[status].fetch_add(1,std::memory_order_relaxed)+1;
    if(kharvox::extendedDiagnosticsEnabled()&&(n<=3||n%2048==0))
        kharvoxnative::log::info(std::format("[AER-WEAPON-DRAW] r268 status={} source={} camera={} eye={} matrixDelta={} auxiliaryCameraDelta={} flags={} count={}",status,matched,camera.poseId,camera.eye,matrixError,cameraError,flags,n));
 }
 if(bindingRecovered){
    static std::atomic<uint64_t> recoveries{};const auto n=++recoveries;
    if(kharvox::extendedDiagnosticsEnabled()&&(n<=4||n%120==0))
        kharvoxnative::log::info(std::format("[AER-WEAPON-BINDING] r289 recovered model={} asset={} camera={} eye={} count={}",model,asset,camera.poseId,camera.eye,n));
 }
 // Observe nearby unclassified models only during explicit capture. This
 // includes candidates for weapon-mounted GUI without changing their draw.
 if(kharvox::pose_trace::active.load(std::memory_order_relaxed)&&model&&view){
    kharvox::AerWorldView observed;float p[12]{},v[14]{};unsigned char f{},b{};
    if(safe_read_bytes(model+0x30,p,sizeof(p))&&safe_read_bytes(model+0xba,&f,1)
        &&safe_read_bytes(model+0xb9,&b,1)&&safe_read_bytes(view+0x940,v,12*sizeof(float))
        &&safe_read_bytes(view+0x8f0,v+12,2*sizeof(float))
        &&KharvoxCameraRecognizeAerWorldView(v,v+12,observed)==kharvox::AerWorldViewResult::Unchanged){
        float distance2=0;for(int i=0;i<3;++i)distance2+=(p[i]-v[i])*(p[i]-v[i]);
        if(distance2<100.f*100.f){
            if(!asset)safe_read_bytes(model,&asset,sizeof(asset));
            kharvox::pose_trace::Event e{};e.kind=kharvox::pose_trace::NearbyModelPose;e.source=model;
            e.revision=asset;e.poseId=observed.poseId;e.eye=observed.eye;e.frame=KharvoxCameraCurrentPresentSerial();
            e.flags=unsigned(f)|(unsigned(b)<<8);e.status=status;e.displayTime=reinterpret_cast<uintptr_t>(_ReturnAddress());
            std::memcpy(e.data,p,sizeof(p));e.data[12]=distance2;kharvox::pose_trace::record(e);
        }
    }
 }
 // Never mutate the shared render model or view. Original projection/depth
 // behavior executes exactly once against a call-local corrected copy.
 // r286: bounded, explicit CPU capture only. Link all four matrices to one
 // invocation; do not write files or change shared render state here.
 const bool audit=kharvox::pose_trace::active.load(std::memory_order_relaxed)&&camera.poseId;
 static std::atomic<uint64_t> auditSequence{};
 const auto auditId=audit?auditSequence.fetch_add(1,std::memory_order_relaxed)+1:0;
 const auto caller=audit?reinterpret_cast<uintptr_t>(_ReturnAddress()):0;
 auto traceMatrix=[&](unsigned kind,uintptr_t address){
    if(!audit)return;
    kharvox::pose_trace::Event e{};
    if(!safe_read_bytes(address,e.data,sizeof(e.data)))return;
    e.kind=kind;e.source=model;e.revision=caller;e.displayTime=auditId;
    e.frame=KharvoxCameraCurrentPresentSerial();e.poseId=camera.poseId;
    e.eye=camera.eye;e.status=status;e.flags=flags;
    kharvox::pose_trace::record(e);
 };
 traceMatrix(kharvox::pose_trace::WeaponMatrixOriginal,model+0xb24);
 traceMatrix(kharvox::pose_trace::WeaponMatrixSelected,reinterpret_cast<uintptr_t>(selected)+0xb24);
 traceMatrix(kharvox::pose_trace::WeaponMatrixView,reinterpret_cast<uintptr_t>(selectedView)+0x11f0);
 realAerModelMatrix(output,selected,selectedView,x,y,z,jitter);
 traceMatrix(kharvox::pose_trace::WeaponMatrixOutput,reinterpret_cast<uintptr_t>(output));
}
static AerBuildViewMatricesFn realAerBuildViewMatrices{};
static bool validateAerWorldProducer(unsigned char* base){
 DWORD64 image{};const auto address=reinterpret_cast<DWORD64>(base+0x1830b80);
 const auto entry=RtlLookupFunctionEntry(address,&image,nullptr);
 if(!entry||image+entry->BeginAddress!=address||entry->EndAddress-entry->BeginAddress!=0x101
    ||((base[entry->UnwindData]>>3)&UNW_FLAG_CHAININFO))return false;
 uint64_t hash=14695981039346656037ull;
 for(size_t i=0;i<0x101;++i)hash=(hash^base[0x1830b80+i])*1099511628211ull;
 return hash==0x4b471325a3428ebfull;
}
static void traceAerWorldProducer(unsigned kind,uintptr_t view,const float* data,size_t count,
    int status,const kharvox::AerWorldView& source,const kharvox::AerWorldView& target){
 if(!kharvox::pose_trace::active.load(std::memory_order_relaxed))return;
 kharvox::pose_trace::Event e{};e.kind=kind;e.source=view;e.status=status;
 e.frame=KharvoxCameraCurrentPresentSerial();e.poseId=target.poseId;e.revision=source.poseId;
 e.eye=target.eye;e.flags=source.poseId?1u:0u;
 std::memcpy(e.data,data,std::min(count,size_t{16})*sizeof(float));
 kharvox::pose_trace::record(e);
}
static void __fastcall hookAerBuildViewMatrices(void* rawView,void* rect,uint64_t altScale){
 const auto view=reinterpret_cast<uintptr_t>(rawView);
 float before[14]{},after[14]{};uint8_t identity=1;
 kharvox::AerWorldView source;
 int status=static_cast<int>(kharvox::AerWorldViewResult::Invalid);
 const bool readable=view&&safe_read_bytes(view+0xD8,&identity,1)&&identity==0
    &&safe_read_bytes(view+0x60,before,12*sizeof(float))
    &&safe_read_bytes(view+0x10,before+12,2*sizeof(float));
 if(readable)status=static_cast<int>(KharvoxCameraRecognizeAerWorldView(before,before+12,source));
 // Observe only. Exactly one engine call preserves its previous-frame matrices.
 realAerBuildViewMatrices(rawView,rect,altScale);
 if(!readable)return;
 const bool unchanged=safe_read_bytes(view+0x60,after,12*sizeof(float))
    &&safe_read_bytes(view+0x10,after+12,2*sizeof(float))
    &&std::memcmp(before,after,sizeof(before))==0;
 if(status==static_cast<int>(kharvox::AerWorldViewResult::Unchanged)){
    status=unchanged?6:7;
    if(unchanged)KharvoxCameraRecordAerWorldSource(source);
 }
 traceAerWorldProducer(kharvox::pose_trace::WorldProducerInput,view,before,14,status,source,source);
 if(unchanged)traceAerWorldProducer(kharvox::pose_trace::WorldProducerOutput,view,after,14,status,source,source);
 if(kharvox::pose_trace::active.load(std::memory_order_relaxed)){
    float matrix[16]{};
    if(safe_read_bytes(view+0x1270,matrix,sizeof(matrix)))
        traceAerWorldProducer(kharvox::pose_trace::WorldProducerMatrix,view,matrix,16,status,source,source);
 }
 static std::array<std::atomic<uint64_t>,8> counts{};
 const auto n=counts[std::clamp(status,0,7)].fetch_add(1,std::memory_order_relaxed)+1;
 if(kharvox::extendedDiagnosticsEnabled()&&(n<=3||n%1024==0))
    kharvoxnative::log::info(std::format("[AER-WORLD-SOURCE] r262 status={} count={} sourcePose={} sourceEye={} (6=completed source observed,7=producer changed source; CPU observation only)",
        status,n,source.poseId,source.eye));
}
