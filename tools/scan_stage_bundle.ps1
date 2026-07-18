param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [string]$OutputPath = ""
)

function Read-U32BE([byte[]]$Bytes, [int]$Offset) {
  return (([uint32]$Bytes[$Offset] -shl 24) -bor
          ([uint32]$Bytes[$Offset + 1] -shl 16) -bor
          ([uint32]$Bytes[$Offset + 2] -shl 8) -bor
          [uint32]$Bytes[$Offset + 3])
}

function Read-U32LE([byte[]]$Bytes, [int]$Offset) {
  return ([uint32]$Bytes[$Offset] -bor
          ([uint32]$Bytes[$Offset + 1] -shl 8) -bor
          ([uint32]$Bytes[$Offset + 2] -shl 16) -bor
          ([uint32]$Bytes[$Offset + 3] -shl 24))
}

function Read-AsciiPreview([byte[]]$Bytes, [int]$Offset, [int]$Length) {
  $builder = New-Object System.Text.StringBuilder
  for ($i = 0; $i -lt $Length; $i++) {
    if ($Offset + $i -ge $Bytes.Length) {
      break
    }
    $c = $Bytes[$Offset + $i]
    [void]$builder.Append($(if ($c -ge 32 -and $c -le 126) { [char]$c } else { "." }))
  }
  return $builder.ToString()
}

function Get-SubfileKind([string]$Magic4) {
  switch ($Magic4) {
    "G1M_" { return "model_g1m" }
    "G1TG" { return "texture_g1t" }
    "G1A_" { return "animation_g1a" }
    "KSHL" { return "kshl_pack" }
    default {
      if ($Magic4 -eq "[glo") { return "render_config_text" }
      return "unknown"
    }
  }
}

function Get-G1MChunkSummary([byte[]]$Bytes, [int]$BaseOffset, [int]$Size) {
  if ($Size -lt 0x18 -or (Read-AsciiPreview $Bytes $BaseOffset 4) -ne "G1M_") {
    return [pscustomobject]@{
      TopVersion = ""
      Unknown10 = ""
      ChunkCountHint = ""
      Chunks = ""
      GeometrySizes = ""
    }
  }

  $topVersion = Read-AsciiPreview $Bytes ($BaseOffset + 4) 4
  $firstChunkOffset = Read-U32BE $Bytes ($BaseOffset + 12)
  $unknown10 = Read-U32BE $Bytes ($BaseOffset + 16)
  $chunkCountHint = Read-U32BE $Bytes ($BaseOffset + 20)
  $cursor = $BaseOffset + [int]$firstChunkOffset
  $end = $BaseOffset + $Size
  $chunks = New-Object System.Collections.Generic.List[string]
  $geometrySizes = New-Object System.Collections.Generic.List[string]
  $guard = 0

  while ($cursor -ge $BaseOffset -and $cursor + 12 -le $end -and $guard -lt 512) {
    $chunkMagic = Read-AsciiPreview $Bytes $cursor 4
    if ($chunkMagic -notmatch '^G1M[A-Z_]$') {
      break
    }

    $chunkVersion = Read-AsciiPreview $Bytes ($cursor + 4) 4
    $chunkSize = Read-U32BE $Bytes ($cursor + 8)
    if ($chunkSize -lt 12 -or $cursor + $chunkSize -gt $end) {
      [void]$chunks.Add(("{0}{1}:invalid_0x{2:X}" -f $chunkMagic, $chunkVersion, $chunkSize))
      break
    }

    [void]$chunks.Add(("{0}{1}:0x{2:X}" -f $chunkMagic, $chunkVersion, $chunkSize))
    if ($chunkMagic -eq "G1MG") {
      [void]$geometrySizes.Add(("0x{0:X}" -f $chunkSize))
    }
    $cursor += [int]$chunkSize
    $guard++
  }

  return [pscustomobject]@{
    TopVersion = $topVersion
    Unknown10 = ("0x{0:X8}" -f $unknown10)
    ChunkCountHint = $chunkCountHint
    Chunks = ($chunks -join ";")
    GeometrySizes = ($geometrySizes -join ";")
  }
}

$resolved = Resolve-Path -LiteralPath $InputPath
$bytes = [IO.File]::ReadAllBytes($resolved.Path)
if ($bytes.Length -lt 8) {
  throw "Container is too small: $($resolved.Path)"
}

$count = Read-U32BE $bytes 0
if ($count -eq 0 -or $count -gt 0x10000) {
  throw "Unreasonable offset-table count $count in $($resolved.Path)"
}

$tableEnd = 4 + ($count * 4)
if ($tableEnd -gt $bytes.Length) {
  throw "Offset table exceeds file length: $($resolved.Path)"
}

$offsets = New-Object System.Collections.Generic.List[uint32]
for ($i = 0; $i -lt $count; $i++) {
  $offset = Read-U32BE $bytes (4 + ($i * 4))
  if ($offset -lt $tableEnd -or $offset -gt $bytes.Length) {
    throw "Invalid subentry offset 0x$('{0:X}' -f $offset) at index $i"
  }
  if ($i -gt 0 -and $offset -lt $offsets[$i - 1]) {
    throw "Offset table is not sorted at index $i"
  }
  $offsets.Add($offset)
}

$rows = New-Object System.Collections.Generic.List[object]
for ($i = 0; $i -lt $count; $i++) {
  $payloadOffset = [int64]$offsets[$i]
  $nextOffset = if ($i + 1 -lt $count) { [int64]$offsets[$i + 1] } else { [int64]$bytes.Length }
  $size = $nextOffset - $payloadOffset
  if ($size -lt 0 -or $payloadOffset + $size -gt $bytes.Length) {
    throw "Invalid subentry $i range"
  }

  $magic4 = Read-AsciiPreview $bytes ([int]$payloadOffset) 4
  $magic8 = Read-AsciiPreview $bytes ([int]$payloadOffset) 8
  $kind = Get-SubfileKind $magic4
  if ($size -eq 0) {
    $kind = "empty"
  }
  $g1m = Get-G1MChunkSummary $bytes ([int]$payloadOffset) ([int]$size)
  $firstU32BE = if ($size -ge 4) { "0x{0:X8}" -f (Read-U32BE $bytes ([int]$payloadOffset)) } else { "" }
  $firstU32LE = if ($size -ge 4) { "0x{0:X8}" -f (Read-U32LE $bytes ([int]$payloadOffset)) } else { "" }

  $rows.Add([pscustomobject]@{
    Index = $i
    Offset = ("0x{0:X8}" -f $payloadOffset)
    Size = $size
    Kind = $kind
    Magic4 = $magic4
    Magic8 = $magic8
    FirstU32BE = $firstU32BE
    FirstU32LE = $firstU32LE
    G1MVersion = $g1m.TopVersion
    G1MUnknown10 = $g1m.Unknown10
    G1MChunkCountHint = $g1m.ChunkCountHint
    G1MChunks = $g1m.Chunks
    G1MGGeometrySizes = $g1m.GeometrySizes
  })
}

if ($OutputPath) {
  $parent = Split-Path -Parent $OutputPath
  if ($parent -and !(Test-Path -LiteralPath $parent)) {
    [void][IO.Directory]::CreateDirectory($parent)
  }
  $rows | Export-Csv -LiteralPath $OutputPath -NoTypeInformation
}

$rows
