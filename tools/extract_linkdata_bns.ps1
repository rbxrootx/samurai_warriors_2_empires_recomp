param(
  [string]$GameDataRoot = "L:\SM2\game_files\data",
  [string]$OutputRoot = "L:\SM2\samurai_warriors_2_empires_recomp\extracted\linkdata_bns",
  [int]$Start = 0,
  [int]$Count = 0,
  [string[]]$Magic = @(),
  [switch]$Extract,
  [switch]$CatalogOnly
)

$idxPath = Join-Path $GameDataRoot "LINKDATA_BNS.IDX"
$lnkPath = Join-Path $GameDataRoot "LINKDATA_BNS.LNK"

if (!(Test-Path -LiteralPath $idxPath) -or !(Test-Path -LiteralPath $lnkPath)) {
  throw "LINKDATA_BNS.IDX/LNK were not found under $GameDataRoot"
}

function Read-U32BE([byte[]]$Bytes, [int]$Offset) {
  return (([uint32]$Bytes[$Offset] -shl 24) -bor
          ([uint32]$Bytes[$Offset + 1] -shl 16) -bor
          ([uint32]$Bytes[$Offset + 2] -shl 8) -bor
          [uint32]$Bytes[$Offset + 3])
}

function Convert-AsciiPreview([byte[]]$Bytes, [int]$Length) {
  $builder = New-Object System.Text.StringBuilder
  for ($i = 0; $i -lt $Length; $i++) {
    $c = $Bytes[$i]
    [void]$builder.Append($(if ($c -ge 32 -and $c -le 126) { [char]$c } else { "." }))
  }
  return $builder.ToString()
}

function Get-EntryExtension([string]$Magic4, [string]$Magic8) {
  switch -Regex ($Magic4) {
    "^G1TG$" { return ".g1t" }
    "^G1M_$" { return ".g1m" }
    "^G1A_$" { return ".g1a" }
    "^LZP2$" { return ".lzp2" }
    "^KSHL$" { return ".kshl" }
    "^\[glo$" { return ".txt" }
    "^00_2$" { return ".cap" }
    "^SDsd$" { return ".hdx" }
    default { return ".bin" }
  }
}

function Get-EntryFolder([string]$Magic4, [string]$Magic8) {
  switch -Regex ($Magic4) {
    "^G1TG$" { return "textures_g1t" }
    "^G1M_$" { return "models_g1m" }
    "^G1A_$" { return "animations_g1a" }
    "^LZP2$" { return "compressed_lzp2" }
    "^KSHL$" { return "kshl_packs" }
    "^\[glo$" { return "global_render_config" }
    "^00_2$" { return "cap_metadata" }
    "^SDsd$" { return "sound_headers" }
    default { return "unknown_bin" }
  }
}

function Copy-EntryBytes([IO.FileStream]$Source, [int64]$Offset, [uint32]$Size, [string]$Path) {
  $parent = Split-Path -Parent $Path
  if (!(Test-Path -LiteralPath $parent)) {
    [void][IO.Directory]::CreateDirectory($parent)
  }

  $buffer = New-Object byte[] (1024 * 1024)
  $remaining = [int64]$Size
  [void]$Source.Seek($Offset, [IO.SeekOrigin]::Begin)
  $dest = [IO.File]::Create($Path)
  try {
    while ($remaining -gt 0) {
      $readSize = [Math]::Min($buffer.Length, $remaining)
      $read = $Source.Read($buffer, 0, [int]$readSize)
      if ($read -le 0) {
        throw "Unexpected EOF while extracting $Path"
      }
      $dest.Write($buffer, 0, $read)
      $remaining -= $read
    }
  }
  finally {
    $dest.Dispose()
  }
}

$idx = [IO.File]::ReadAllBytes($idxPath)
$idxMagic = [Text.Encoding]::ASCII.GetString($idx, 0, 4)
if ($idxMagic -ne "SM4L") {
  throw "Unexpected LINKDATA_BNS.IDX magic '$idxMagic'"
}

$entryCount = Read-U32BE $idx 4
$totalSectors = Read-U32BE $idx 8
$sectorSize = 0x800
$end = if ($Count -gt 0) { [Math]::Min($Start + $Count, $entryCount) } else { $entryCount }
$magicFilter = @{}
foreach ($m in $Magic) {
  $magicFilter[$m.ToUpperInvariant()] = $true
}

if (!(Test-Path -LiteralPath $OutputRoot)) {
  [void][IO.Directory]::CreateDirectory($OutputRoot)
}

$catalogPath = Join-Path $OutputRoot "catalog.csv"
$catalog = New-Object System.Collections.Generic.List[object]
$lnk = [IO.File]::OpenRead($lnkPath)
try {
  for ($i = $Start; $i -lt $end; $i++) {
    $entryOffset = 16 + ($i * 16)
    $sector = Read-U32BE $idx $entryOffset
    $allocSectors = Read-U32BE $idx ($entryOffset + 4)
    $size = Read-U32BE $idx ($entryOffset + 8)
    $flags = Read-U32BE $idx ($entryOffset + 12)
    $offset = [int64]$sector * $sectorSize
    $header = New-Object byte[] 16
    if ($size -gt 0 -and $offset -lt $lnk.Length) {
      [void]$lnk.Seek($offset, [IO.SeekOrigin]::Begin)
      [void]$lnk.Read($header, 0, [Math]::Min($header.Length, [int]$size))
    }

    $magic4 = Convert-AsciiPreview $header 4
    $magic8 = Convert-AsciiPreview $header 8
    if ($magicFilter.Count -gt 0 -and !$magicFilter.ContainsKey($magic4.ToUpperInvariant())) {
      continue
    }

    $ext = Get-EntryExtension $magic4 $magic8
    $folder = Get-EntryFolder $magic4 $magic8
    $safeMagic = ($magic8 -replace '[^A-Za-z0-9_\[\]-]', '_')
    $fileName = "entry_{0:D4}_{1}_size_{2:X8}{3}" -f $i, $safeMagic, $size, $ext
    $relativePath = Join-Path $folder $fileName
    $outputPath = Join-Path $OutputRoot $relativePath

    $catalog.Add([pscustomobject]@{
      Index = $i
      Offset = ("0x{0:X}" -f $offset)
      Sector = $sector
      AllocSectors = $allocSectors
      Size = $size
      Flags = $flags
      Magic4 = $magic4
      Magic8 = $magic8
      Extension = $ext
      Folder = $folder
      RelativePath = $relativePath
      HeaderHex = (($header | ForEach-Object { $_.ToString("X2") }) -join " ")
    })

    if ($Extract -and !$CatalogOnly -and $size -gt 0) {
      Copy-EntryBytes $lnk $offset $size $outputPath
    }
  }
}
finally {
  $lnk.Dispose()
}

$catalog | Export-Csv -LiteralPath $catalogPath -NoTypeInformation

[pscustomobject]@{
  EntriesTotal = $entryCount
  EntriesSelected = $catalog.Count
  TotalSectors = $totalSectors
  ArchiveBytes = [int64]$totalSectors * $sectorSize
  Catalog = $catalogPath
  OutputRoot = $OutputRoot
  Extracted = [bool]($Extract -and !$CatalogOnly)
}
