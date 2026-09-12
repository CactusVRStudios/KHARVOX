param([Parameter(Mandatory=$true)][string]$CaptureDirectory)
$ErrorActionPreference='Stop'
$meta=Get-Content -Raw -LiteralPath (Join-Path $CaptureDirectory 'capture.json') | ConvertFrom-Json
$rows=@(Import-Csv -LiteralPath (Join-Path $CaptureDirectory 'events.csv'))
if($meta.schema -ne 1 -or $rows.Count -ne $meta.events){throw 'Invalid or incomplete capture'}
$frequency=[double]$meta.qpcFrequency
if($frequency -le 0){throw 'Invalid QPC frequency'}
$names=@{'1'='Located';'2'='Programmed';'3'='Camera';'4'='CacheRecorded';'5'='Submitted';'6'='EndBegin';'7'='EndReturn';'8'='PresentBegin';'9'='HeadApplied'}
$counts=@{};foreach($group in ($rows | Group-Object kind)){$counts[$names[$group.Name]]=$group.Count}
$starts=@($rows | Where-Object { $_.kind -eq '6' -and (([int]$_.flags -band 2) -eq 0) })
$intervals=@(for($i=1;$i -lt $starts.Count;$i++){
    # Exclude transitions/menu gaps: only consecutive XR frame serials.
    if(([long]$starts[$i].frame-[long]$starts[$i-1].frame) -eq 1){
        ([long]$starts[$i].qpc-[long]$starts[$i-1].qpc)*1000/$frequency
    }
})
[pscustomobject]@{
    Events=$rows.Count;Dropped=$meta.dropped;Counts=$counts
    ConsecutiveProjectionIntervals=$intervals.Count
    MeanEndFrameIntervalMs=($intervals | Measure-Object -Average).Average
    MaxEndFrameIntervalMs=($intervals | Measure-Object -Maximum).Maximum
    CpuOnly=$true;GpuFrameToPoseMappingVerified=$false
} | ConvertTo-Json -Depth 4
if($meta.dropped -ne 0){Write-Warning 'Dropped records: this trace cannot establish complete event ordering.'}
