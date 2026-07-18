param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [string]$OutputPath = "",
  [string]$OutputDirectory = "",
  [switch]$Force
)

function Read-U32LE([byte[]]$Bytes, [int]$Offset) {
  return [BitConverter]::ToUInt32($Bytes, $Offset)
}

function Get-OutputExtension([byte[]]$Bytes) {
  if ($Bytes.Length -lt 4) {
    return ".bin"
  }

  $magic4 = [Text.Encoding]::ASCII.GetString($Bytes, 0, 4)
  switch ($magic4) {
    "G1TG" { return ".g1t" }
    "G1M_" { return ".g1m" }
    "G1A_" { return ".g1a" }
    "KSHL" { return ".kshl" }
    default {
      if ($magic4 -eq "[glo") {
        return ".txt"
      }
      return ".bin"
    }
  }
}

function Expand-Lzp2File([string]$SourcePath, [string]$DestPath, [bool]$Overwrite) {
  $bytes = [IO.File]::ReadAllBytes($SourcePath)
  if ($bytes.Length -lt 0x10) {
    throw "LZP2 file is too small: $SourcePath"
  }

  $magic = [Text.Encoding]::ASCII.GetString($bytes, 0, 4)
  if ($magic -ne "LZP2") {
    throw "Input is not an LZP2 stream: $SourcePath"
  }

  $expectedSize = Read-U32LE $bytes 8
  $payloadSize = Read-U32LE $bytes 12
  if ($payloadSize + 0x10 -gt $bytes.Length) {
    throw "LZP2 payload size exceeds file length: $SourcePath"
  }

  $output = New-Object byte[] $expectedSize
  $inPos = 0x10
  $outPos = 0
  while ($inPos -lt $bytes.Length -and $outPos -lt $expectedSize) {
    $control = $bytes[$inPos]

    if (($control -band 0xC0) -eq 0) {
      $literalCount = [int]$control
      $inPos++
      for ($i = 0; $i -lt $literalCount -and $outPos -lt $expectedSize; $i++) {
        if ($inPos -ge $bytes.Length) {
          throw "Unexpected EOF in LZP2 literal run: $SourcePath"
        }
        $output[$outPos++] = $bytes[$inPos++]
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
      if ($inPos -ge $bytes.Length) {
        throw "Unexpected EOF in LZP2 back-reference: $SourcePath"
      }
      $offset = [int]$bytes[$inPos] + (([int]($control -band 0x07)) -shl 8) + 1
      $inPos++

      if ($offset -le 0 -or $offset -gt $outPos) {
        throw "Invalid LZP2 back-reference offset $offset at input 0x$('{0:X}' -f $inPos): $SourcePath"
      }

      for ($i = 0; $i -lt $copyLength -and $outPos -lt $expectedSize; $i++) {
        $output[$outPos] = $output[$outPos - $offset]
        $outPos++
      }
      continue
    }

    $inPos++
    if ($inPos + 1 -ge $bytes.Length) {
      throw "Unexpected EOF in LZP2 RLE run: $SourcePath"
    }
    $amount = [int]$bytes[$inPos] + 3 + (([int]($control -band 0x3F)) -shl 8)
    $inPos++
    $value = $bytes[$inPos]
    $inPos++
    for ($i = 0; $i -le $amount -and $outPos -lt $expectedSize; $i++) {
      $output[$outPos++] = $value
    }
  }

  if ($outPos -ne $expectedSize) {
    throw "LZP2 decompressed $outPos bytes, expected ${expectedSize}: $SourcePath"
  }

  if (!$DestPath) {
    $baseName = [IO.Path]::GetFileNameWithoutExtension($SourcePath)
    $ext = Get-OutputExtension $output
    $DestPath = if ($OutputDirectory) {
      Join-Path $OutputDirectory ($baseName + ".decompressed" + $ext)
    } else {
      Join-Path ([IO.Path]::GetDirectoryName($SourcePath)) ($baseName + ".decompressed" + $ext)
    }
  }

  if ((Test-Path -LiteralPath $DestPath) -and !$Overwrite) {
    throw "Output already exists, pass -Force to overwrite: $DestPath"
  }

  $parent = Split-Path -Parent $DestPath
  if ($parent -and !(Test-Path -LiteralPath $parent)) {
    [void][IO.Directory]::CreateDirectory($parent)
  }

  [IO.File]::WriteAllBytes($DestPath, $output)
  [pscustomobject]@{
    Input = $SourcePath
    Output = $DestPath
    CompressedBytes = $bytes.Length
    PayloadBytes = $payloadSize
    DecompressedBytes = $expectedSize
    OutputExtension = Get-OutputExtension $output
  }
}

$resolvedInput = Resolve-Path -LiteralPath $InputPath
if ((Get-Item -LiteralPath $resolvedInput).PSIsContainer) {
  $results = @()
  foreach ($file in Get-ChildItem -LiteralPath $resolvedInput -Recurse -File -Filter "*.lzp2") {
    $results += Expand-Lzp2File $file.FullName "" ([bool]$Force)
  }
  $results
} else {
  Expand-Lzp2File $resolvedInput.Path $OutputPath ([bool]$Force)
}
