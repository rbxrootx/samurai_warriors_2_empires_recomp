param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [string]$OutputDirectory = "",
  [switch]$Force
)

function Read-U32BE([byte[]]$Bytes, [int]$Offset) {
  return (([uint32]$Bytes[$Offset] -shl 24) -bor
          ([uint32]$Bytes[$Offset + 1] -shl 16) -bor
          ([uint32]$Bytes[$Offset + 2] -shl 8) -bor
          [uint32]$Bytes[$Offset + 3])
}

function Convert-AsciiPreview([byte[]]$Bytes, [int]$Offset, [int]$Length) {
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

function Get-Extension([string]$Magic4) {
  switch ($Magic4) {
    "G1TG" { return ".g1t" }
    "G1M_" { return ".g1m" }
    "G1A_" { return ".g1a" }
    "KSHL" { return ".kshl" }
    default {
      if ($Magic4 -eq "[glo") { return ".txt" }
      return ".bin"
    }
  }
}

$bytes = [IO.File]::ReadAllBytes($InputPath)
if ($bytes.Length -lt 8) {
  throw "Container is too small: $InputPath"
}

$count = Read-U32BE $bytes 0
if ($count -eq 0 -or $count -gt 0x10000) {
  throw "Unreasonable offset-table count $count in $InputPath"
}

$tableEnd = 4 + ($count * 4)
if ($tableEnd -gt $bytes.Length) {
  throw "Offset table exceeds file length: $InputPath"
}

$offsets = New-Object System.Collections.Generic.List[uint32]
for ($i = 0; $i -lt $count; $i++) {
  $offset = Read-U32BE $bytes (4 + ($i * 4))
  if ($offset -lt $tableEnd -or $offset -gt $bytes.Length) {
    throw "Invalid subentry offset 0x$('{0:X}' -f $offset) at index $i in $InputPath"
  }
  if ($i -gt 0 -and $offset -lt $offsets[$i - 1]) {
    throw "Offset table is not sorted at index $i in $InputPath"
  }
  $offsets.Add($offset)
}

if (!$OutputDirectory) {
  $OutputDirectory = Join-Path ([IO.Path]::GetDirectoryName($InputPath)) ([IO.Path]::GetFileNameWithoutExtension($InputPath) + ".split")
}
if (!(Test-Path -LiteralPath $OutputDirectory)) {
  [void][IO.Directory]::CreateDirectory($OutputDirectory)
}

$catalog = New-Object System.Collections.Generic.List[object]
for ($i = 0; $i -lt $count; $i++) {
  $payloadOffset = [int64]$offsets[$i]
  $nextOffset = if ($i + 1 -lt $count) { [int64]$offsets[$i + 1] } else { [int64]$bytes.Length }
  $size = $nextOffset - $payloadOffset
  if ($size -lt 0 -or $payloadOffset + $size -gt $bytes.Length) {
    throw "Invalid subentry $i range in $InputPath"
  }

  if ($size -eq 0) {
    $magic4 = ""
    $magic8 = ""
  } else {
    $magic4 = Convert-AsciiPreview $bytes ([int]$payloadOffset) 4
    $magic8 = Convert-AsciiPreview $bytes ([int]$payloadOffset) 8
  }
  $ext = Get-Extension $magic4
  $safeMagic = ($magic8 -replace '[^A-Za-z0-9_\[\]-]', '_')
  $fileName = "sub_{0:D4}_off_{1:X8}_{2}_size_{3:X8}{4}" -f $i, $payloadOffset, $safeMagic, $size, $ext
  $outputPath = Join-Path $OutputDirectory $fileName

  if ((Test-Path -LiteralPath $outputPath) -and !$Force) {
    throw "Output already exists, pass -Force to overwrite: $outputPath"
  }

  $subBytes = New-Object byte[] $size
  if ($size -gt 0) {
    [Array]::Copy($bytes, [int]$payloadOffset, $subBytes, 0, [int]$size)
  }
  [IO.File]::WriteAllBytes($outputPath, $subBytes)

  $catalog.Add([pscustomobject]@{
    Index = $i
    Offset = ("0x{0:X}" -f $payloadOffset)
    Size = $size
    Magic4 = $magic4
    Magic8 = $magic8
    RelativePath = [IO.Path]::GetFileName($outputPath)
  })
}

$catalogPath = Join-Path $OutputDirectory "catalog.csv"
$catalog | Export-Csv -LiteralPath $catalogPath -NoTypeInformation

[pscustomobject]@{
  Input = $InputPath
  OutputDirectory = $OutputDirectory
  Catalog = $catalogPath
  Count = $count
  TableEnd = ("0x{0:X}" -f $tableEnd)
}
