#requires -Version 7.0
param(
 [string]$Executable = (Join-Path $PSScriptRoot '../../Saved/VRWindowsArchive/Windows/ACUnreal/Binaries/Win64/ACUnreal.exe'),
 [string]$OutputRoot = (Join-Path $PSScriptRoot '../../Saved/PCPerformance/Review'),
 [string[]]$Scenes = @('outdoor','indoor','effects'),
 [ValidateSet('baseline','optimized')][string[]]$Variants = @('baseline','optimized'),
 [ValidateSet('scene','particles','cached-world','particle-distance','particle-idle','cached-actors','doorway-geometry','setup-metadata','cpu-render','animation-buffers','mobile-light-permutation','caul-ucm')][string]$Comparison = 'scene',
 [switch]$Editor,
 [switch]$MobilePreview,
 [switch]$CameraMotion,
 [int]$Width = 2560,
 [int]$Height = 1440
)
$ErrorActionPreference='Stop'
if($Comparison -eq 'mobile-light-permutation' -and !$MobilePreview){throw 'The local-light permutation comparison requires MobilePreview.'}
foreach ($scene in $Scenes) {
 if ($scene -notin @('outdoor','indoor','effects','caul','caul-tower')) { throw "Unknown scene $scene" }
 if($Comparison -eq 'caul-ucm' -and $scene -ne 'caul-tower'){throw 'Use the caul-tower scene for the stationary UCM comparison.'}
 foreach ($variant in $Variants) {
  $runDirectory=Join-Path $OutputRoot "$scene-$variant"
  if (Test-Path (Join-Path $runDirectory "Saved/Performance/$scene/summary.json")) { throw "Run already exists: $runDirectory" }
  New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
  $enabled=if($variant -eq 'optimized'){1}else{0}
  $settings=switch($Comparison) {
   'caul-ucm' { 'ace.UI.IndexedLookups 1,ace.Render.ReusePrimitiveBuffers 1' }
   # Keep actor caching enabled in both phases; isolate the mobile light shader
   # policy that can rebuild cached draws whenever animated parts move near lights.
   'mobile-light-permutation' { "ace.Render.CachedActorDraws 1,r.Mobile.Forward.LocalLightsSinglePermutation $enabled" }
   'animation-buffers' { "ace.Animation.ReusePoseBuffers $enabled" }
   'cached-actors' { "ace.Render.CachedActorDraws $enabled" }
   'doorway-geometry' { "ace.Render.CacheDoorwayGeometry $enabled" }
   'setup-metadata' { "ace.Dat.CacheSetupMetadata $enabled" }
   'cpu-render' { "ace.Render.CachedActorDraws $enabled,ace.Render.CacheDoorwayGeometry $enabled,ace.Dat.CacheSetupMetadata $enabled" }
   'particles' { "ace.UI.IndexedLookups 1,ace.Render.ReusePrimitiveBuffers 1,ace.Particles.ActivePrefix $enabled" }
   'cached-world' { "ace.UI.IndexedLookups 1,ace.Render.ReusePrimitiveBuffers 1,ace.Render.CachedWorldDraws $enabled" }
   'particle-distance' { "ace.UI.IndexedLookups 1,ace.Render.ReusePrimitiveBuffers 1,ace.Particles.DistanceCulling $enabled" }
   'particle-idle' { "ace.UI.IndexedLookups 1,ace.Render.ReusePrimitiveBuffers 1,ace.Particles.DistanceCulling 1,ace.Particles.IdleTickInterval $(if($enabled){'.1'}else{'0'})" }
   default { "ace.UI.IndexedLookups $enabled,ace.Render.ReusePrimitiveBuffers $enabled" }
  }
  $commands="r.VSync 0,t.MaxFPS 0,r.SetRes ${Width}x${Height}w,$settings,ace.PerfScene $scene"
  $arguments=@('-nohmd','-unattended','-nosound','-ACEPerfQuit',"-UserDir=`"$runDirectory/`"","-abslog=`"$runDirectory/run.log`"","-ExecCmds=`"$commands`"")
  if($CameraMotion){$arguments+='-ACEPerfCameraMotion'}
  if($Comparison -eq 'caul-ucm'){$arguments+=@("-ACEPerfCaulUCM=$variant","-ACEPerfResolution=${Width}x${Height}")}
  if($Editor){
   $arguments=@([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../ACUnreal.uproject')),'-game')+$arguments
  }
  if($MobilePreview){
   if(!$Editor){throw 'MobilePreview requires Editor mode.'}
   $arguments+=@('-FeatureLevelES31','-ini:Engine:[/Script/Engine.RendererSettings]:r.MobileHDR=False')
  }
  [ordered]@{
   startedUtc=[DateTime]::UtcNow.ToString('o')
   scene=$scene; comparison=$Comparison; variant=$variant
   executable=[IO.Path]::GetFullPath($Executable)
   editor=[bool]$Editor; mobilePreview=[bool]$MobilePreview
   cameraMotion=[bool]$CameraMotion; width=$Width; height=$Height
   commands=$commands; warmupSeconds=45; sampleSeconds=30
   synthetic=$true; nativeHeadset=$false
  } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory 'run-settings.json') -Encoding utf8
  $process=Start-Process -FilePath $Executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
  Write-Host "Measuring $scene / $variant (PID $($process.Id))"
  if (!$process.WaitForExit(150000)) {
   $actual=Get-Process -Id $process.Id -ErrorAction SilentlyContinue
   if($actual -and $actual.Path -eq [IO.Path]::GetFullPath($Executable)) { Stop-Process -Id $actual.Id }
   throw "Benchmark timed out. See $runDirectory/run.log"
  }
  if($process.ExitCode -ne 0){throw "Benchmark failed: exit $($process.ExitCode). See $runDirectory/run.log"}
  $report=Join-Path $runDirectory "Saved/Performance/$scene/summary.json"
  if(!(Test-Path $report)){throw "No completed benchmark report: $runDirectory/run.log"}
  Get-Content -LiteralPath $report
 }
}
