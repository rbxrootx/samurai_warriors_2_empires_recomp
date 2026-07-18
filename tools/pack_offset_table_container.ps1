param(
  [Parameter(Mandatory = $true)]
  [string]$SplitDirectory,
  [string]$OutputPath = "",
  [int]$Alignment = 0x10,
  [switch]$Force
)

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
  if ($Alignment -le 0) {
    return $Value
  }
  $remainder = $Value % $Alignment
  if ($remainder -eq 0) {
    return $Value
  }
  return $Value + ($Alignment - $remainder)
}

function Write-Padding([IO.Stream]$Stream, [int64]$TargetOffset) {
  while ($Stream.Position -lt $TargetOffset) {
    $Stream.WriteByte(0)
  }
}

$catalogPath = Join-Path $SplitDirectory "catalog.csv"
if (!(Test-Path -LiteralPath $catalogPath)) {
  throw "Missing split catalog: $catalogPath"
}

$entries = Import-Csv -LiteralPath $catalogPath | Sort-Object {[int]$_.Index}
if (!$OutputPath) {
  $OutputPath = Join-Path $SplitDirectory ((Split-Path -Leaf $SplitDirectory) + ".repacked.offset_table.bin")
}
if ((Test-Path -LiteralPath $OutputPath) -and !$Force) {
  throw "Output already exists, pass -Force to overwrite: $OutputPath"
}

$payloads = New-Object System.Collections.Generic.List[object]
foreach ($entry in $entries) {
  $path = Join-Path $SplitDirectory $entry.RelativePath
  if (!(Test-Path -LiteralPath $path)) {
    throw "Missing split payload for index $($entry.Index): $path"
  }
  $payloads.Add([pscustomobject]@{
    Index = [int]$entry.Index
    Path = $path
    Bytes = [IO.File]::ReadAllBytes($path)
  })
}

$count = [uint32]$payloads.Count
$tableEnd = [int64](4 + ($count * 4))
$cursor = Align-Value $tableEnd $Alignment

$records = New-Object System.Collections.Generic.List[object]
foreach ($payload in $payloads) {
  $cursor = Align-Value $cursor $Alignment
  $records.Add([pscustomobject]@{
    Offset = [int64]$cursor
    Bytes = $payload.Bytes
  })
  $cursor += $payload.Bytes.Length
}

$parent = Split-Path -Parent $OutputPath
if ($parent -and !(Test-Path -LiteralPath $parent)) {
  [void][IO.Directory]::CreateDirectory($parent)
}

$stream = [IO.File]::Create($OutputPath)
try {
  Write-U32BE $stream $count

  foreach ($record in $records) {
    Write-U32BE $stream ([uint32]$record.Offset)
  }

  for ($i = 0; $i -lt $records.Count; $i++) {
    $record = $records[$i]
    Write-Padding $stream $record.Offset
    if ($record.Bytes.Length -gt 0) {
      $stream.Write($record.Bytes, 0, $record.Bytes.Length)
    }
  }
}
finally {
  $stream.Dispose()
}

[pscustomobject]@{
  Input = $SplitDirectory
  Output = $OutputPath
  Count = $count
  Alignment = $Alignment
  Bytes = (Get-Item -LiteralPath $OutputPath).Length
}
