param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [string]$OutputPath = "",
  [switch]$Force
)

function Write-U32LE([IO.Stream]$Stream, [uint32]$Value) {
  $bytes = [byte[]](
    ($Value -band 0xFF),
    (($Value -shr 8) -band 0xFF),
    (($Value -shr 16) -band 0xFF),
    (($Value -shr 24) -band 0xFF)
  )
  $Stream.Write($bytes, 0, 4)
}

$resolvedInput = Resolve-Path -LiteralPath $InputPath
$sourceBytes = [IO.File]::ReadAllBytes($resolvedInput.Path)
if ($sourceBytes.Length -gt [uint32]::MaxValue) {
  throw "Input is too large for this LZP2 header: $InputPath"
}

if (!$OutputPath) {
  $dir = [IO.Path]::GetDirectoryName($resolvedInput.Path)
  $baseName = [IO.Path]::GetFileNameWithoutExtension($resolvedInput.Path)
  $OutputPath = Join-Path $dir ($baseName + ".literal.lzp2")
}
if ((Test-Path -LiteralPath $OutputPath) -and !$Force) {
  throw "Output already exists, pass -Force to overwrite: $OutputPath"
}

$payload = New-Object IO.MemoryStream
try {
  $pos = 0
  while ($pos -lt $sourceBytes.Length) {
    $run = [Math]::Min(0x3F, $sourceBytes.Length - $pos)
    $payload.WriteByte([byte]$run)
    $payload.Write($sourceBytes, $pos, $run)
    $pos += $run
  }
  $payload.WriteByte(0)

  if ($payload.Length -gt [uint32]::MaxValue) {
    throw "Compressed payload is too large for this LZP2 header: $InputPath"
  }

  $parent = Split-Path -Parent $OutputPath
  if ($parent -and !(Test-Path -LiteralPath $parent)) {
    [void][IO.Directory]::CreateDirectory($parent)
  }

  $out = [IO.File]::Create($OutputPath)
  try {
    $magic = [Text.Encoding]::ASCII.GetBytes("LZP2")
    $out.Write($magic, 0, 4)
    Write-U32LE $out 0x3F8147AE
    Write-U32LE $out ([uint32]$sourceBytes.Length)
    Write-U32LE $out ([uint32]$payload.Length)
    $payload.Position = 0
    $payload.CopyTo($out)
  }
  finally {
    $out.Dispose()
  }
}
finally {
  $payload.Dispose()
}

[pscustomobject]@{
  Input = $resolvedInput.Path
  Output = $OutputPath
  DecompressedBytes = $sourceBytes.Length
  PayloadBytes = (Get-Item -LiteralPath $OutputPath).Length - 0x10
  TotalBytes = (Get-Item -LiteralPath $OutputPath).Length
}
