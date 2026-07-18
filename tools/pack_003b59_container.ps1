param(
  [Parameter(Mandatory = $true)]
  [string]$SplitDirectory,
  [string]$OutputPath = "",
  [uint32]$Unknown0C = 0,
  [int]$FinalAlignment = 0x10,
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
  $OutputPath = Join-Path $SplitDirectory ((Split-Path -Leaf $SplitDirectory) + ".repacked.003b59.bin")
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
$tableOffset = [uint32]0x10
$tableEnd = $tableOffset + ($count * 8)
$dataOffset = [int64]$tableEnd
$dataOffset = Align-Value $dataOffset 0x10

$records = New-Object System.Collections.Generic.List[object]
$cursor = $dataOffset
foreach ($payload in $payloads) {
  $cursor = Align-Value $cursor 0x10
  $records.Add([pscustomobject]@{
    Offset = [int64]$cursor
    Size = [uint32]$payload.Bytes.Length
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
  Write-U32BE $stream 0x00033B59
  Write-U32BE $stream $count
  Write-U32BE $stream $tableOffset
  Write-U32BE $stream $Unknown0C

  foreach ($record in $records) {
    Write-U32BE $stream ([uint32]($record.Offset / 0x10))
    Write-U32BE $stream $record.Size
  }

  for ($i = 0; $i -lt $records.Count; $i++) {
    $record = $records[$i]
    Write-Padding $stream $record.Offset
    $stream.Write($record.Bytes, 0, $record.Bytes.Length)
  }

  Write-Padding $stream (Align-Value $stream.Position $FinalAlignment)
}
finally {
  $stream.Dispose()
}

[pscustomobject]@{
  Input = $SplitDirectory
  Output = $OutputPath
  Count = $count
  FinalAlignment = $FinalAlignment
  Bytes = (Get-Item -LiteralPath $OutputPath).Length
}
