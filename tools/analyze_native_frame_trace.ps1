param([Parameter(Mandatory=$true)][string]$TracePath)
$ErrorActionPreference='Stop'
$path=(Resolve-Path -LiteralPath $TracePath).Path
$reader=[IO.File]::OpenText($path)
$frames=@{};$submits=@{};$api=@{};$gaps=@{};$eventCount=0L;$header=''
try {
 $header=$reader.ReadLine()
 if($header -notmatch '^# r179 capture='){throw 'Not an r179 frame trace'}
 if($reader.ReadLine() -notlike 'id*frame*startNs*'){throw 'Missing trace columns'}
 while($null -ne ($line=$reader.ReadLine())){
  $v=$line.Split("`t");if($v.Length -ne 18){throw 'Truncated trace row'}
  ++$eventCount;$id=$v[0];$frame=$v[1];$link=$v[4];$name=$v[8]
  if(!$frames.ContainsKey($frame)){$frames[$frame]=[ordered]@{frame=$frame;cpuSlots=@{};events=0;complete=$false}}
  $f=$frames[$frame];$f.events++
  if($name -eq 'frame-completed'){$f.complete=$true}
  if($name -eq 'cpu-slot-total'){$f.cpuSlots[$v[10]]=[ordered]@{calls=$v[11];wallNs=$v[12];threadCpuNs=$v[13];cycles=$v[14];threadSamples=$v[15]}}
  if($name -eq 'cpu-frame-summary'){$f.intervalNs=$v[12];$f.rootToCompleteNs=$v[13]}
  if($name.StartsWith('coverage-gap')){if(!$gaps.ContainsKey($name)){$gaps[$name]=0};$gaps[$name]++}
  if($name -eq 'submit'){$submits[$link]=[ordered]@{id=$link;frame=$frame;queue=$v[10];fence=$v[11];batches=$v[12];version=$v[13];commands=@();waits=@();signals=@()}}
  if($name -eq 'submit-command' -and $submits.ContainsKey($link)){$submits[$link].commands+=,[ordered]@{batch=$v[10];order=$v[11];commandBuffer=$v[12];deviceMask=$v[13]}}
  if(($name -eq 'submit-wait' -or $name -eq 'submit-signal') -and $submits.ContainsKey($link)){
   $entry=[ordered]@{batch=$v[10];order=$v[11];semaphore=$v[12];stageMask=$v[13];value=$v[14];hasTimelineInfo=$v[15];deviceIndex=$v[16]}
   if($name -eq 'submit-wait'){$submits[$link].waits+=,$entry}else{$submits[$link].signals+=,$entry}
  }
  if($submits.ContainsKey($id)){$s=$submits[$id];$s.origin=$name;$s.cpuStartNs=$v[2];$s.cpuEndNs=$v[3];$s.result=$v[9]}
  if($name.StartsWith('driver:') -or $name.StartsWith('adapter:')){if(!$api.ContainsKey($name)){$api[$name]=[ordered]@{calls=0;inclusiveNs=[decimal]0}};$api[$name].calls++;$api[$name].inclusiveNs+=([decimal]$v[3]-[decimal]$v[2])}
 }
} finally {$reader.Dispose()}
$slots=@('Root','Original','Doubled','SnapshotBind','SnapshotCopy','StorageSeed','DeviceIdle','XrWaitFrame','XrCopyCompletion','XrEndFrame','ReplayDescriptors','CaptureImageInputs','PrepareImageLayouts','ReplayPass','ValidateStorageDraw','GpuInputWait','XrQueueLock','XrQueueSubmit','XrQueueWait','ForwardDescriptors','ForwardDraw','ForwardOther','StorageSelection','ReplayVertex','ReplayPipeline','ReplayDraw','SnapshotClassLookup','SnapshotMemoLookup','SnapshotMemoRestore','SnapshotMemoMiss','SnapshotForward')
$metadata=@{};foreach($m in [regex]::Matches($header,'(\w+)=([^ ]+)')){$metadata[$m.Groups[1].Value]=$m.Groups[2].Value}
$quality=$metadata.reason -eq 'four-frames' -and $metadata.frames -eq '4' -and $metadata.dropped -eq '0' -and $metadata.registryDropped -eq '0' -and $metadata.openScopesAtStop -eq '0'
$report=[ordered]@{trace=$path;metadata=$metadata;captureBoundaryComplete=$quality;coverageGaps=$gaps;rows=$eventCount;slotNames=$slots;frames=@($frames.Values|Sort-Object {[uint64]$_.frame});submissions=@($submits.Values|Sort-Object {[uint64]$_.id});api=$api;limitations='CPU spans include instrumentation and may nest. Do not sum adapter and driver time. GPU timestamps in companion TSVs use another clock: do not subtract CPU and GPU timestamps. A complete capture boundary does not prove complete Vulkan extension or shader/host-memory access coverage. Raw mapped-memory writes and shader occupancy are not captured. Use PID + frame + command buffer + span to join companion GPU data.'}
$output=[IO.Path]::ChangeExtension($path,'.summary.json')
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $output -Encoding utf8
[pscustomobject]@{Summary=$output;Rows=$eventCount;Frames=$frames.Count;Submissions=$submits.Count;CaptureBoundaryComplete=$quality;CoverageGapKinds=$gaps.Count}
