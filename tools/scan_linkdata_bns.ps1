param(
  [string]$GameDataRoot = "L:\SM2\game_files\data",
  [int]$Start = 0,
  [int]$Count = 0
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
  $chars = New-Object System.Text.StringBuilder
  for ($i = 0; $i -lt $Length; $i++) {
    $c = $Bytes[$i]
    [void]$chars.Append($(if ($c -ge 32 -and $c -le 126) { [char]$c } else { "." }))
  }
  return $chars.ToString()
}

$idx = [IO.File]::ReadAllBytes($idxPath)
$magic = [Text.Encoding]::ASCII.GetString($idx, 0, 4)
if ($magic -ne "SM4L") {
  throw "Unexpected LINKDATA_BNS.IDX magic '$magic'"
}

$entryCount = Read-U32BE $idx 4
$totalSectors = Read-U32BE $idx 8
$sectorSize = 0x800
$end = if ($Count -gt 0) { [Math]::Min($Start + $Count, $entryCount) } else { $entryCount }

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

    [pscustomobject]@{
      Index = $i
      Offset = ("0x{0:X}" -f $offset)
      Sector = $sector
      AllocSectors = $allocSectors
      Size = $size
      Flags = $flags
      Magic4 = Convert-AsciiPreview $header 4
      Magic8 = Convert-AsciiPreview $header 8
      HeaderHex = (($header | ForEach-Object { $_.ToString("X2") }) -join " ")
    }
  }
}
finally {
  $lnk.Dispose()
}

Write-Verbose "LINKDATA_BNS entries=$entryCount sectors=$totalSectors bytes=$([int64]$totalSectors * $sectorSize)"
