param(
  [string]$GameDataRoot = "L:\SM2\game_files\data",
  [string]$CatalogPath = "L:\SM2\samurai_warriors_2_empires_recomp\extracted\linkdata_bns_catalog\catalog.csv",
  [string]$PatchRoot = "L:\SM2\samurai_warriors_2_empires_recomp\mods\linkdata_bns_patch",
  [string]$OutputDirectory = "L:\SM2\samurai_warriors_2_empires_recomp\out\modded_linkdata_bns",
  [switch]$DryRun,
  [switch]$Force
)

$sectorSize = 0x800
$idxPath = Join-Path $GameDataRoot "LINKDATA_BNS.IDX"
$lnkPath = Join-Path $GameDataRoot "LINKDATA_BNS.LNK"

function Write-U32BE([IO.Stream]$Stream, [uint32]$Value) {
  $bytes = [byte[]](
    (($Value -shr 24) -band 0xFF),
    (($Value -shr 16) -band 0xFF),
    (($Value -shr 8) -band 0xFF),
    ($Value -band 0xFF)
  )
  $Stream.Write($bytes, 0, 4)
}

function Align-Value([int64]$Value, [int64]$Alignment) {
  $remainder = $Value % $Alignment
  if ($remainder -eq 0) {
    return $Value
  }
  return $Value + ($Alignment - $remainder)
}

function Copy-Range([IO.Stream]$Source, [IO.Stream]$Dest, [int64]$Offset, [int64]$Size) {
  $buffer = New-Object byte[] (1024 * 1024)
  [void]$Source.Seek($Offset, [IO.SeekOrigin]::Begin)
  $remaining = $Size
  while ($remaining -gt 0) {
    $want = [Math]::Min($buffer.Length, $remaining)
    $read = $Source.Read($buffer, 0, [int]$want)
    if ($read -le 0) {
      throw "Unexpected EOF while copying archive bytes at 0x$('{0:X}' -f $Offset)"
    }
    $Dest.Write($buffer, 0, $read)
    $remaining -= $read
  }
}

function Write-PaddingToOffset([IO.Stream]$Stream, [int64]$TargetOffset) {
  while ($Stream.Position -lt $TargetOffset) {
    $Stream.WriteByte(0)
  }
}

if (!(Test-Path -LiteralPath $idxPath) -or !(Test-Path -LiteralPath $lnkPath)) {
  throw "LINKDATA_BNS.IDX/LNK were not found under $GameDataRoot"
}
if (!(Test-Path -LiteralPath $CatalogPath)) {
  throw "Missing catalog: $CatalogPath"
}

$rows = Import-Csv -LiteralPath $CatalogPath | Sort-Object {[int]$_.Index}
if ($rows.Count -eq 0) {
  throw "Catalog contains no rows: $CatalogPath"
}

$replacements = New-Object System.Collections.Generic.List[object]
$cursor = [int64]0
foreach ($row in $rows) {
  $patchPath = Join-Path $PatchRoot $row.RelativePath
  $usePatch = Test-Path -LiteralPath $patchPath
  $originalAllocSectors = [uint32]$row.AllocSectors
  $size = if ($usePatch) {
    (Get-Item -LiteralPath $patchPath).Length
  } else {
    [int64]$row.Size
  }
  $allocSectors = if ($usePatch) {
    if ($size -eq 0) { [uint32]0 } else { [uint32]([Math]::Ceiling($size / [double]$sectorSize)) }
  } else {
    $originalAllocSectors
  }

  $replacements.Add([pscustomobject]@{
    Index = [int]$row.Index
    OriginalOffset = [Convert]::ToInt64(($row.Offset -replace '^0x', ''), 16)
    OriginalSize = [int64]$row.Size
    OriginalAllocSectors = $originalAllocSectors
    Flags = [uint32]$row.Flags
    RelativePath = $row.RelativePath
    PatchPath = $patchPath
    UsePatch = $usePatch
    Sector = [uint32]($cursor / $sectorSize)
    AllocSectors = $allocSectors
    Size = [uint32]$size
    EndOffset = $cursor + ([int64]$allocSectors * $sectorSize)
  })
  $cursor += [int64]$allocSectors * $sectorSize
}

$totalSectors = [uint32]($cursor / $sectorSize)
$patchCount = @($replacements | Where-Object { $_.UsePatch }).Count

if ($DryRun) {
  [pscustomobject]@{
    Entries = $replacements.Count
    PatchedEntries = $patchCount
    OutputSectors = $totalSectors
    OutputBytes = $cursor
    OutputDirectory = $OutputDirectory
  }
  return
}

if (!(Test-Path -LiteralPath $OutputDirectory)) {
  [void][IO.Directory]::CreateDirectory($OutputDirectory)
}

$outIdx = Join-Path $OutputDirectory "LINKDATA_BNS.IDX"
$outLnk = Join-Path $OutputDirectory "LINKDATA_BNS.LNK"
foreach ($path in @($outIdx, $outLnk)) {
  if ((Test-Path -LiteralPath $path) -and !$Force) {
    throw "Output already exists, pass -Force to overwrite: $path"
  }
}

$lnkSource = [IO.File]::OpenRead($lnkPath)
$lnkDest = [IO.File]::Create($outLnk)
try {
  foreach ($entry in $replacements) {
    $expectedOffset = [int64]$entry.Sector * $sectorSize
    if ($lnkDest.Position -ne $expectedOffset) {
      throw "Internal output alignment error at entry $($entry.Index)"
    }

    if ($entry.UsePatch) {
      $patch = [IO.File]::OpenRead($entry.PatchPath)
      try {
        $patch.CopyTo($lnkDest)
      }
      finally {
        $patch.Dispose()
      }
    } else {
      $copySize = [int64]$entry.OriginalAllocSectors * $sectorSize
      if ($copySize -gt 0) {
        Copy-Range $lnkSource $lnkDest $entry.OriginalOffset $copySize
      }
    }
    Write-PaddingToOffset $lnkDest $entry.EndOffset
  }
}
finally {
  $lnkDest.Dispose()
  $lnkSource.Dispose()
}

$idxDest = [IO.File]::Create($outIdx)
try {
  $magic = [Text.Encoding]::ASCII.GetBytes("SM4L")
  $idxDest.Write($magic, 0, 4)
  Write-U32BE $idxDest ([uint32]$replacements.Count)
  Write-U32BE $idxDest $totalSectors
  Write-U32BE $idxDest 0

  foreach ($entry in $replacements) {
    Write-U32BE $idxDest $entry.Sector
    Write-U32BE $idxDest $entry.AllocSectors
    Write-U32BE $idxDest $entry.Size
    Write-U32BE $idxDest $entry.Flags
  }
}
finally {
  $idxDest.Dispose()
}

[pscustomobject]@{
  Entries = $replacements.Count
  PatchedEntries = $patchCount
  OutputIDX = $outIdx
  OutputLNK = $outLnk
  OutputSectors = $totalSectors
  OutputBytes = (Get-Item -LiteralPath $outLnk).Length
}
