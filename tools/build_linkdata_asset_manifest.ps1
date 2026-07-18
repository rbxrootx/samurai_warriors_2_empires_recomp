param(
  [string]$GameDataRoot = "L:\SM2\game_files\data",
  [int]$Start = 0,
  [int]$Count = 0,
  [string]$OutputPath = "",
  [switch]$DecodeLzp2,
  [int]$MaxEntryBytes = 67108864,
  [int]$MaxDecodedBytes = 67108864
)

$idxPath = Join-Path $GameDataRoot "LINKDATA_BNS.IDX"
$lnkPath = Join-Path $GameDataRoot "LINKDATA_BNS.LNK"

if (!(Test-Path -LiteralPath $idxPath) -or !(Test-Path -LiteralPath $lnkPath)) {
  throw "LINKDATA_BNS.IDX/LNK were not found under $GameDataRoot"
}

function Read-U32BE([byte[]]$Bytes, [int]$Offset) {
  if ($Offset + 4 -gt $Bytes.Length) { return 0 }
  return (([uint32]$Bytes[$Offset] -shl 24) -bor
          ([uint32]$Bytes[$Offset + 1] -shl 16) -bor
          ([uint32]$Bytes[$Offset + 2] -shl 8) -bor
          [uint32]$Bytes[$Offset + 3])
}

function Read-U32LE([byte[]]$Bytes, [int]$Offset) {
  if ($Offset + 4 -gt $Bytes.Length) { return 0 }
  return ([uint32]$Bytes[$Offset] -bor
          ([uint32]$Bytes[$Offset + 1] -shl 8) -bor
          ([uint32]$Bytes[$Offset + 2] -shl 16) -bor
          ([uint32]$Bytes[$Offset + 3] -shl 24))
}

function Read-AsciiPreview([byte[]]$Bytes, [int]$Offset, [int]$Length) {
  $builder = New-Object System.Text.StringBuilder
  for ($i = 0; $i -lt $Length; $i++) {
    if ($Offset + $i -ge $Bytes.Length) { break }
    $c = $Bytes[$Offset + $i]
    [void]$builder.Append($(if ($c -ge 32 -and $c -le 126) { [char]$c } else { "." }))
  }
  return $builder.ToString()
}

function Read-EntryBytes([IO.FileStream]$Source, [int64]$Offset, [uint32]$Size, [int]$LimitBytes) {
  if ($Size -eq 0 -or $Size -gt [uint32]$LimitBytes) {
    return $null
  }

  $bytes = New-Object byte[] ([int]$Size)
  [void]$Source.Seek($Offset, [IO.SeekOrigin]::Begin)
  $total = 0
  while ($total -lt $bytes.Length) {
    $read = $Source.Read($bytes, $total, $bytes.Length - $total)
    if ($read -le 0) {
      throw "Unexpected EOF while reading archive bytes at 0x$('{0:X}' -f $Offset)"
    }
    $total += $read
  }
  return $bytes
}

function Get-KindFromMagic([string]$Magic4) {
  switch ($Magic4) {
    "G1M_" { return "model_g1m" }
    "G1TG" { return "texture_g1t" }
    "G1A_" { return "animation_g1a" }
    "LZP2" { return "compressed_lzp2" }
    "KSHL" { return "kshl_pack" }
    "00_2" { return "cap_metadata" }
    default {
      if ($Magic4 -eq "[glo") { return "render_config_text" }
      if ($Magic4 -eq "") { return "empty" }
      return "unknown"
    }
  }
}

function Expand-Lzp2Bytes([byte[]]$Bytes, [int]$MaxBytes) {
  if ($Bytes.Length -lt 0x10 -or (Read-AsciiPreview $Bytes 0 4) -ne "LZP2") {
    throw "Input is not an LZP2 stream"
  }

  $expectedSize = Read-U32LE $Bytes 8
  $payloadSize = Read-U32LE $Bytes 12
  if ($expectedSize -gt [uint32]$MaxBytes) {
    throw "LZP2 output 0x$('{0:X}' -f $expectedSize) exceeds MaxDecodedBytes"
  }
  if ($payloadSize + 0x10 -gt $Bytes.Length) {
    throw "LZP2 payload size exceeds entry length"
  }

  $output = New-Object byte[] ([int]$expectedSize)
  $inPos = 0x10
  $outPos = 0
  while ($inPos -lt $Bytes.Length -and $outPos -lt $expectedSize) {
    $control = $Bytes[$inPos]

    if (($control -band 0xC0) -eq 0) {
      $literalCount = [int]$control
      $inPos++
      for ($i = 0; $i -lt $literalCount -and $outPos -lt $expectedSize; $i++) {
        if ($inPos -ge $Bytes.Length) { throw "Unexpected EOF in LZP2 literal run" }
        $output[$outPos++] = $Bytes[$inPos++]
      }
      continue
    }

    if (($control -band 0x80) -ne 0) {
      $copyLength = 3
      if (($control -band 0x08) -ne 0) { $copyLength += 1 }
      if (($control -band 0x10) -ne 0) { $copyLength += 2 }
      if (($control -band 0x20) -ne 0) { $copyLength += 4 }
      if (($control -band 0x40) -ne 0) { $copyLength += 8 }

      $inPos++
      if ($inPos -ge $Bytes.Length) { throw "Unexpected EOF in LZP2 back-reference" }
      $offset = [int]$Bytes[$inPos] + (([int]($control -band 0x07)) -shl 8) + 1
      $inPos++
      if ($offset -le 0 -or $offset -gt $outPos) {
        throw "Invalid LZP2 back-reference offset $offset"
      }

      for ($i = 0; $i -lt $copyLength -and $outPos -lt $expectedSize; $i++) {
        $output[$outPos] = $output[$outPos - $offset]
        $outPos++
      }
      continue
    }

    $inPos++
    if ($inPos + 1 -ge $Bytes.Length) { throw "Unexpected EOF in LZP2 RLE run" }
    $amount = [int]$Bytes[$inPos] + 3 + (([int]($control -band 0x3F)) -shl 8)
    $inPos++
    $value = $Bytes[$inPos]
    $inPos++
    for ($i = 0; $i -le $amount -and $outPos -lt $expectedSize; $i++) {
      $output[$outPos++] = $value
    }
  }

  if ($outPos -ne $expectedSize) {
    throw "LZP2 decompressed $outPos bytes, expected $expectedSize"
  }

  return $output
}

function New-Summary {
  return @{
    ContainerKind = ""
    SubfileCount = 0
    ModelCount = 0
    TextureContainerCount = 0
    TextureImageCount = 0
    AnimationCount = 0
    CapCount = 0
    RenderConfigCount = 0
    KshlCount = 0
    EmptyCount = 0
    UnknownCount = 0
    G1MGCount = 0
    G1MGGeometrySizes = New-Object System.Collections.Generic.List[string]
    SubKinds = @{}
    Notes = New-Object System.Collections.Generic.List[string]
  }
}

function Add-SubKind([hashtable]$Summary, [string]$Kind) {
  if (!$Summary["SubKinds"].ContainsKey($Kind)) {
    $Summary["SubKinds"][$Kind] = 0
  }
  $Summary["SubKinds"][$Kind]++
}

function Get-G1MGeometrySizes([byte[]]$Bytes, [int]$Offset, [int]$Size) {
  $sizes = New-Object System.Collections.Generic.List[string]
  if ($Size -lt 0x18 -or (Read-AsciiPreview $Bytes $Offset 4) -ne "G1M_") {
    return $sizes
  }

  $firstChunkOffset = Read-U32BE $Bytes ($Offset + 12)
  $cursor = $Offset + [int]$firstChunkOffset
  $end = $Offset + $Size
  $guard = 0
  while ($cursor -ge $Offset -and $cursor + 12 -le $end -and $guard -lt 512) {
    $chunkMagic = Read-AsciiPreview $Bytes $cursor 4
    if ($chunkMagic -notmatch '^G1M[A-Z_]$') { break }

    $chunkSize = Read-U32BE $Bytes ($cursor + 8)
    if ($chunkSize -lt 12 -or $cursor + $chunkSize -gt $end) { break }
    if ($chunkMagic -eq "G1MG") {
      [void]$sizes.Add(("0x{0:X}" -f $chunkSize))
    }
    $cursor += [int]$chunkSize
    $guard++
  }
  return $sizes
}

function Get-G1TTextureCount([byte[]]$Bytes, [int]$Offset, [int]$Size) {
  if ($Size -lt 0x18 -or (Read-AsciiPreview $Bytes $Offset 4) -ne "G1TG") {
    return 0
  }
  return [int](Read-U32BE $Bytes ($Offset + 16))
}

function Add-BufferToSummary([hashtable]$Summary, [byte[]]$Bytes, [int]$Offset, [int]$Size) {
  if ($Size -eq 0) {
    $Summary["EmptyCount"]++
    Add-SubKind $Summary "empty"
    return
  }

  $magic4 = Read-AsciiPreview $Bytes $Offset 4
  $kind = Get-KindFromMagic $magic4
  Add-SubKind $Summary $kind

  switch ($kind) {
    "model_g1m" {
      $Summary["ModelCount"]++
      $geometrySizes = Get-G1MGeometrySizes $Bytes $Offset $Size
      $Summary["G1MGCount"] += $geometrySizes.Count
      foreach ($geometrySize in $geometrySizes) {
        [void]$Summary["G1MGGeometrySizes"].Add($geometrySize)
      }
    }
    "texture_g1t" {
      $Summary["TextureContainerCount"]++
      $Summary["TextureImageCount"] += Get-G1TTextureCount $Bytes $Offset $Size
    }
    "animation_g1a" { $Summary["AnimationCount"]++ }
    "cap_metadata" { $Summary["CapCount"]++ }
    "render_config_text" { $Summary["RenderConfigCount"]++ }
    "kshl_pack" { $Summary["KshlCount"]++ }
    "unknown" { $Summary["UnknownCount"]++ }
  }
}

function Get-003B59Entries([byte[]]$Bytes) {
  if ($Bytes.Length -lt 0x10 -or (Read-U32BE $Bytes 0) -ne 0x00033B59) {
    return $null
  }

  $count = [int](Read-U32BE $Bytes 4)
  $tableOffset = [int](Read-U32BE $Bytes 8)
  if ($count -lt 0 -or $count -gt 0x10000 -or $tableOffset -lt 0x10 -or $tableOffset + ($count * 8) -gt $Bytes.Length) {
    return $null
  }

  $entries = New-Object System.Collections.Generic.List[object]
  for ($i = 0; $i -lt $count; $i++) {
    $record = $tableOffset + ($i * 8)
    $payloadOffset = [int](Read-U32BE $Bytes $record) * 0x10
    $size = [int](Read-U32BE $Bytes ($record + 4))
    if ($payloadOffset -lt 0 -or $payloadOffset + $size -gt $Bytes.Length) {
      return $null
    }
    $entries.Add([pscustomobject]@{ Offset = $payloadOffset; Size = $size })
  }
  return $entries
}

function Get-OffsetTableEntries([byte[]]$Bytes) {
  if ($Bytes.Length -lt 8) { return $null }

  $count = [int](Read-U32BE $Bytes 0)
  if ($count -le 0 -or $count -gt 0x10000) {
    return $null
  }

  $tableEnd = 4 + ($count * 4)
  if ($tableEnd -gt $Bytes.Length) {
    return $null
  }

  $offsets = New-Object System.Collections.Generic.List[int]
  for ($i = 0; $i -lt $count; $i++) {
    $offset = [int](Read-U32BE $Bytes (4 + ($i * 4)))
    if ($offset -lt $tableEnd -or $offset -gt $Bytes.Length) {
      return $null
    }
    if ($i -gt 0 -and $offset -lt $offsets[$i - 1]) {
      return $null
    }
    $offsets.Add($offset)
  }

  $entries = New-Object System.Collections.Generic.List[object]
  for ($i = 0; $i -lt $count; $i++) {
    $payloadOffset = $offsets[$i]
    $nextOffset = if ($i + 1 -lt $count) { $offsets[$i + 1] } else { $Bytes.Length }
    $size = $nextOffset - $payloadOffset
    if ($size -lt 0) { return $null }
    $entries.Add([pscustomobject]@{ Offset = $payloadOffset; Size = $size })
  }
  return $entries
}

function Analyze-Payload([byte[]]$Bytes) {
  $summary = New-Summary
  $topMagic4 = Read-AsciiPreview $Bytes 0 4
  $topKind = Get-KindFromMagic $topMagic4

  $table003b59 = Get-003B59Entries $Bytes
  if ($null -ne $table003b59) {
    $summary["ContainerKind"] = "texture_table_003b59"
    $summary["SubfileCount"] = $table003b59.Count
    foreach ($entry in $table003b59) {
      Add-BufferToSummary $summary $Bytes ([int]$entry.Offset) ([int]$entry.Size)
    }
    return $summary
  }

  $offsetTable = Get-OffsetTableEntries $Bytes
  if ($null -ne $offsetTable) {
    $summary["ContainerKind"] = "offset_table"
    $summary["SubfileCount"] = $offsetTable.Count
    foreach ($entry in $offsetTable) {
      Add-BufferToSummary $summary $Bytes ([int]$entry.Offset) ([int]$entry.Size)
    }
    return $summary
  }

  $summary["ContainerKind"] = "direct_$topKind"
  $summary["SubfileCount"] = if ($topKind -eq "unknown" -or $topKind -eq "empty") { 0 } else { 1 }
  Add-BufferToSummary $summary $Bytes 0 $Bytes.Length
  return $summary
}

function Convert-SubKindsToText([hashtable]$SubKinds) {
  $parts = New-Object System.Collections.Generic.List[string]
  foreach ($key in ($SubKinds.Keys | Sort-Object)) {
    [void]$parts.Add("$key=$($SubKinds[$key])")
  }
  return $parts -join ";"
}

$idx = [IO.File]::ReadAllBytes($idxPath)
$idxMagic = [Text.Encoding]::ASCII.GetString($idx, 0, 4)
if ($idxMagic -ne "SM4L") {
  throw "Unexpected LINKDATA_BNS.IDX magic '$idxMagic'"
}

$entryCount = Read-U32BE $idx 4
$sectorSize = 0x800
$end = if ($Count -gt 0) { [Math]::Min($Start + $Count, $entryCount) } else { $entryCount }
$rows = New-Object System.Collections.Generic.List[object]

$lnk = [IO.File]::OpenRead($lnkPath)
try {
  for ($i = $Start; $i -lt $end; $i++) {
    $entryOffset = 16 + ($i * 16)
    $sector = Read-U32BE $idx $entryOffset
    $allocSectors = Read-U32BE $idx ($entryOffset + 4)
    $size = Read-U32BE $idx ($entryOffset + 8)
    $flags = Read-U32BE $idx ($entryOffset + 12)
    $offset = [int64]$sector * $sectorSize

    $header = New-Object byte[] 32
    if ($size -gt 0 -and $offset -lt $lnk.Length) {
      [void]$lnk.Seek($offset, [IO.SeekOrigin]::Begin)
      [void]$lnk.Read($header, 0, [Math]::Min($header.Length, [int]$size))
    }

    $topMagic4 = Read-AsciiPreview $header 0 4
    $topMagic8 = Read-AsciiPreview $header 0 8
    $topKind = Get-KindFromMagic $topMagic4
    $payloadMagic4 = $topMagic4
    $payloadMagic8 = $topMagic8
    $decodeStatus = ""
    $decodeError = ""
    $lzp2ExpectedBytes = ""
    $lzp2PayloadBytes = ""
    $summary = New-Summary

    $entryBytes = Read-EntryBytes $lnk $offset $size $MaxEntryBytes
    if ($null -eq $entryBytes) {
      $decodeStatus = "skipped_entry_too_large_or_empty"
      $summary["ContainerKind"] = "not_scanned"
      if ($size -eq 0) {
        $summary["EmptyCount"] = 1
      }
    } else {
      $payloadBytes = $entryBytes
      if ($topMagic4 -eq "LZP2") {
        $lzp2ExpectedBytes = "0x{0:X8}" -f (Read-U32LE $entryBytes 8)
        $lzp2PayloadBytes = "0x{0:X8}" -f (Read-U32LE $entryBytes 12)
        if ($DecodeLzp2) {
          try {
            $payloadBytes = Expand-Lzp2Bytes $entryBytes $MaxDecodedBytes
            $decodeStatus = "decoded"
            $payloadMagic4 = Read-AsciiPreview $payloadBytes 0 4
            $payloadMagic8 = Read-AsciiPreview $payloadBytes 0 8
          } catch {
            $decodeStatus = "decode_failed"
            $decodeError = $_.Exception.Message
          }
        } else {
          $decodeStatus = "not_requested"
        }
      }

      if ($topMagic4 -eq "LZP2" -and $decodeStatus -ne "decoded") {
        $summary["ContainerKind"] = "compressed_lzp2"
      } else {
        $summary = Analyze-Payload $payloadBytes
      }
    }

    $rows.Add([pscustomobject]@{
      Index = $i
      Offset = ("0x{0:X}" -f $offset)
      Sector = $sector
      AllocSectors = $allocSectors
      Size = $size
      Flags = $flags
      TopMagic4 = $topMagic4
      TopMagic8 = $topMagic8
      TopKind = $topKind
      Lzp2ExpectedBytes = $lzp2ExpectedBytes
      Lzp2PayloadBytes = $lzp2PayloadBytes
      Lzp2Status = $decodeStatus
      Lzp2Error = $decodeError
      PayloadMagic4 = $payloadMagic4
      PayloadMagic8 = $payloadMagic8
      ContainerKind = $summary["ContainerKind"]
      SubfileCount = $summary["SubfileCount"]
      SubKinds = Convert-SubKindsToText $summary["SubKinds"]
      ModelCount = $summary["ModelCount"]
      TextureContainerCount = $summary["TextureContainerCount"]
      TextureImageCount = $summary["TextureImageCount"]
      AnimationCount = $summary["AnimationCount"]
      CapCount = $summary["CapCount"]
      RenderConfigCount = $summary["RenderConfigCount"]
      KshlCount = $summary["KshlCount"]
      EmptyCount = $summary["EmptyCount"]
      UnknownCount = $summary["UnknownCount"]
      G1MGCount = $summary["G1MGCount"]
      G1MGGeometrySizes = ($summary["G1MGGeometrySizes"] -join ";")
    })
  }
}
finally {
  $lnk.Dispose()
}

if ($OutputPath) {
  $parent = Split-Path -Parent $OutputPath
  if ($parent -and !(Test-Path -LiteralPath $parent)) {
    [void][IO.Directory]::CreateDirectory($parent)
  }
  $rows | Export-Csv -LiteralPath $OutputPath -NoTypeInformation
}

$rows
