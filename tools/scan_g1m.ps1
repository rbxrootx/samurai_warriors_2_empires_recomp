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

function Read-U16BE([byte[]]$Bytes, [int]$Offset) {
  return (([uint16]$Bytes[$Offset] -shl 8) -bor [uint16]$Bytes[$Offset + 1])
}

function Read-Ascii([byte[]]$Bytes, [int]$Offset, [int]$Length) {
  if ($Offset + $Length -gt $Bytes.Length) {
    return ""
  }
  return [Text.Encoding]::ASCII.GetString($Bytes, $Offset, $Length)
}

function Read-F32BE([byte[]]$Bytes, [int]$Offset) {
  if ($Offset + 4 -gt $Bytes.Length) {
    return [double]::NaN
  }

  $little = [byte[]]@($Bytes[$Offset + 3], $Bytes[$Offset + 2], $Bytes[$Offset + 1], $Bytes[$Offset])
  return [BitConverter]::ToSingle($little, 0)
}

function Get-G1MGSectionName([uint32]$Magic) {
  switch ($Magic) {
    0x00010001 { return "section1" }
    0x00010002 { return "materials" }
    0x00010003 { return "section3" }
    0x00010004 { return "vertex_buffers" }
    0x00010005 { return "vertex_attributes" }
    0x00010006 { return "joint_palettes" }
    0x00010007 { return "index_buffers" }
    0x00010008 { return "submeshes" }
    0x00010009 { return "mesh_groups" }
    default { return "unknown_section" }
  }
}

function Get-G1MGVADatatypeName([byte]$Value) {
  switch ($Value) {
    0x00 { return "Float_x1" }
    0x01 { return "Float_x2" }
    0x02 { return "Float_x3" }
    0x03 { return "Float_x4" }
    0x05 { return "UByte_x4" }
    0x07 { return "UShort_x4" }
    0x09 { return "UInt_x4" }
    0x0A { return "HalfFloat_x2" }
    0x0B { return "HalfFloat_x4" }
    0x0D { return "NormUByte_x4" }
    0xFF { return "Dummy" }
    default { return "Unknown" }
  }
}

function Get-G1MGVASemanticName([byte]$Value) {
  switch ($Value) {
    0x00 { return "Position" }
    0x01 { return "JointWeight" }
    0x02 { return "JointIndex" }
    0x03 { return "Normal" }
    0x04 { return "PSize" }
    0x05 { return "UV" }
    0x06 { return "Tangent" }
    0x07 { return "Binormal" }
    0x08 { return "TessellationFactor" }
    0x09 { return "PosTransform" }
    0x0A { return "Color" }
    0x0B { return "Fog" }
    0x0C { return "Depth" }
    0x0D { return "Sample" }
    default { return "Unknown" }
  }
}

function Get-G1MGIndexTypeName([uint32]$Value) {
  switch ($Value) {
    0x08 { return "u8" }
    0x10 { return "u16" }
    0x20 { return "u32" }
    default { return "unknown" }
  }
}

function Align-Value([int]$Value, [int]$Alignment) {
  return (($Value + $Alignment - 1) -band (-bnot ($Alignment - 1)))
}

function Add-G1MGVertexAttributeRows(
  [System.Collections.Generic.List[object]]$Rows,
  [byte[]]$Bytes,
  [string]$Path,
  [int]$SectionOffset,
  [int]$SectionSize,
  [int]$SectionItemCount
) {
  $sectionEnd = $SectionOffset + $SectionSize
  $cursor = $SectionOffset + 12
  for ($setIndex = 0; $setIndex -lt $SectionItemCount; $setIndex++) {
    if ($cursor + 4 -gt $sectionEnd) {
      break
    }

    $setOffset = $cursor
    $bufferRefCount = [int](Read-U32BE $Bytes $cursor)
    $cursor += 4
    $bufferRefs = New-Object System.Collections.Generic.List[uint32]
    for ($i = 0; $i -lt $bufferRefCount -and $cursor + 4 -le $sectionEnd; $i++) {
      $bufferRefs.Add((Read-U32BE $Bytes $cursor))
      $cursor += 4
    }

    if ($cursor + 4 -gt $sectionEnd) {
      break
    }

    $attributeCount = [int](Read-U32BE $Bytes $cursor)
    $cursor += 4
    $Rows.Add([pscustomobject]@{
      File = $Path
      Offset = ("0x{0:X8}" -f $setOffset)
      Magic = "G1MG.vertex_attribute_set"
      Version = ""
      Size = $cursor - $setOffset
      NextOffset = ("0x{0:X8}" -f $cursor)
      Notes = ("set={0}; buffer_ref_count={1}; attribute_count={2}; buffers=[{3}]" -f $setIndex, $bufferRefCount, $attributeCount, ($bufferRefs -join ","))
    })

    for ($attributeIndex = 0; $attributeIndex -lt $attributeCount; $attributeIndex++) {
      if ($cursor + 8 -gt $sectionEnd) {
        break
      }

      $attributeOffset = $cursor
      $bufferRefIndex = [int](Read-U16BE $Bytes $cursor)
      $streamOffset = Read-U16BE $Bytes ($cursor + 2)
      $dataType = $Bytes[$cursor + 4]
      $semantic = $Bytes[$cursor + 6]
      $layer = $Bytes[$cursor + 7]
      $resolvedBuffer = ""
      if ($bufferRefIndex -ge 0 -and $bufferRefIndex -lt $bufferRefs.Count) {
        $resolvedBuffer = $bufferRefs[$bufferRefIndex]
      }
      $cursor += 8

      $Rows.Add([pscustomobject]@{
        File = $Path
        Offset = ("0x{0:X8}" -f $attributeOffset)
        Magic = "G1MG.vertex_attribute"
        Version = ""
        Size = 8
        NextOffset = ("0x{0:X8}" -f $cursor)
        Notes = ("set={0}; attribute={1}; semantic={2}(0x{3:X2}); data_type={4}(0x{5:X2}); buffer_ref={6}; buffer={7}; stream_offset={8}; layer={9}" -f $setIndex, $attributeIndex, (Get-G1MGVASemanticName $semantic), $semantic, (Get-G1MGVADatatypeName $dataType), $dataType, $bufferRefIndex, $resolvedBuffer, $streamOffset, $layer)
      })
    }
  }
}

function Add-G1MGIndexBufferRows(
  [System.Collections.Generic.List[object]]$Rows,
  [byte[]]$Bytes,
  [string]$Path,
  [int]$SectionOffset,
  [int]$SectionSize,
  [int]$SectionItemCount,
  [string]$G1MGVersion
) {
  $sectionEnd = $SectionOffset + $SectionSize
  $cursor = $SectionOffset + 12
  $recordHeaderSize = 8
  if ($G1MGVersion -gt "0040") {
    $recordHeaderSize = 12
  }

  for ($i = 0; $i -lt $SectionItemCount; $i++) {
    if ($cursor + $recordHeaderSize -gt $sectionEnd) {
      break
    }

    $recordOffset = $cursor
    $indexCount = Read-U32BE $Bytes $cursor
    $dataType = Read-U32BE $Bytes ($cursor + 4)
    $typeName = Get-G1MGIndexTypeName $dataType
    $bitWidth = 0
    switch ($dataType) {
      0x08 { $bitWidth = 1 }
      0x10 { $bitWidth = 2 }
      0x20 { $bitWidth = 4 }
    }
    $dataOffset = $cursor + $recordHeaderSize
    $dataBytes = [int]$indexCount * $bitWidth
    $next = Align-Value ($dataOffset + $dataBytes) 4
    if ($bitWidth -eq 0 -or $next -gt $sectionEnd) {
      $next = $sectionEnd
    }

    $Rows.Add([pscustomobject]@{
      File = $Path
      Offset = ("0x{0:X8}" -f $recordOffset)
      Magic = "G1MG.index_buffer"
      Version = ""
      Size = $next - $recordOffset
      NextOffset = ("0x{0:X8}" -f $next)
      Notes = ("index_buffer={0}; index_count={1}; data_type={2}(0x{3:X2}); data_offset=0x{4:X8}; data_bytes={5}" -f $i, $indexCount, $typeName, $dataType, $dataOffset, $dataBytes)
    })

    $cursor = $next
  }
}

function Add-G1MGSubmeshRows(
  [System.Collections.Generic.List[object]]$Rows,
  [byte[]]$Bytes,
  [string]$Path,
  [int]$SectionOffset,
  [int]$SectionSize,
  [int]$SectionItemCount
) {
  $sectionEnd = $SectionOffset + $SectionSize
  $cursor = $SectionOffset + 12
  $recordSize = 56
  for ($i = 0; $i -lt $SectionItemCount; $i++) {
    if ($cursor + $recordSize -gt $sectionEnd) {
      break
    }

    $submeshType = Read-U32BE $Bytes $cursor
    $vertexBufferIndex = Read-U32BE $Bytes ($cursor + 4)
    $bonePaletteIndex = Read-U32BE $Bytes ($cursor + 8)
    $materialIndex = Read-U32BE $Bytes ($cursor + 24)
    $indexBufferIndex = Read-U32BE $Bytes ($cursor + 28)
    $primType = Read-U32BE $Bytes ($cursor + 36)
    $vertexBufferOffset = Read-U32BE $Bytes ($cursor + 40)
    $vertexCount = Read-U32BE $Bytes ($cursor + 44)
    $indexBufferOffset = Read-U32BE $Bytes ($cursor + 48)
    $indexCount = Read-U32BE $Bytes ($cursor + 52)
    $next = $cursor + $recordSize

    $Rows.Add([pscustomobject]@{
      File = $Path
      Offset = ("0x{0:X8}" -f $cursor)
      Magic = "G1MG.submesh"
      Version = ""
      Size = $recordSize
      NextOffset = ("0x{0:X8}" -f $next)
      Notes = ("submesh={0}; type={1}; vertex_buffer={2}; bone_palette={3}; material={4}; index_buffer={5}; prim_type={6}; vertex_offset={7}; vertex_count={8}; index_offset={9}; index_count={10}" -f $i, $submeshType, $vertexBufferIndex, $bonePaletteIndex, $materialIndex, $indexBufferIndex, $primType, $vertexBufferOffset, $vertexCount, $indexBufferOffset, $indexCount)
    })

    $cursor = $next
  }
}

function Add-G1MGSectionRows(
  [System.Collections.Generic.List[object]]$Rows,
  [byte[]]$Bytes,
  [string]$Path,
  [int]$ChunkOffset,
  [int]$ChunkSize,
  [string]$G1MGVersion
) {
  $chunkEnd = $ChunkOffset + $ChunkSize
  $headerOffset = $ChunkOffset + 12
  $headerSize = 36
  if ($headerOffset + $headerSize -gt $chunkEnd -or $headerOffset + $headerSize -gt $Bytes.Length) {
    $Rows.Add([pscustomobject]@{
      File = $Path
      Offset = ("0x{0:X8}" -f $headerOffset)
      Magic = "G1MG_HEADER"
      Version = ""
      Size = 0
      NextOffset = ""
      Notes = "invalid or truncated G1MG header"
    })
    return
  }

  $platform = Read-U32BE $Bytes $headerOffset
  $reserved = Read-U32BE $Bytes ($headerOffset + 4)
  $minX = Read-F32BE $Bytes ($headerOffset + 8)
  $minY = Read-F32BE $Bytes ($headerOffset + 12)
  $minZ = Read-F32BE $Bytes ($headerOffset + 16)
  $maxX = Read-F32BE $Bytes ($headerOffset + 20)
  $maxY = Read-F32BE $Bytes ($headerOffset + 24)
  $maxZ = Read-F32BE $Bytes ($headerOffset + 28)
  $sectionCount = Read-U32BE $Bytes ($headerOffset + 32)

  $Rows.Add([pscustomobject]@{
    File = $Path
    Offset = ("0x{0:X8}" -f $headerOffset)
    Magic = "G1MG_HEADER"
    Version = ""
    Size = $headerSize
    NextOffset = ("0x{0:X8}" -f ($headerOffset + $headerSize))
    Notes = ("platform=0x{0:X8}; reserved=0x{1:X8}; section_count={2}; bounds=({3:F3},{4:F3},{5:F3})..({6:F3},{7:F3},{8:F3})" -f $platform, $reserved, $sectionCount, $minX, $minY, $minZ, $maxX, $maxY, $maxZ)
  })

  $sectionOffset = $headerOffset + $headerSize
  for ($i = 0; $i -lt $sectionCount; $i++) {
    if ($sectionOffset + 12 -gt $chunkEnd -or $sectionOffset + 12 -gt $Bytes.Length) {
      $Rows.Add([pscustomobject]@{
        File = $Path
        Offset = ("0x{0:X8}" -f $sectionOffset)
        Magic = "G1MG_SECTION"
        Version = ""
        Size = 0
        NextOffset = ""
        Notes = ("section_index={0}; truncated section header" -f $i)
      })
      break
    }

    $sectionMagic = Read-U32BE $Bytes $sectionOffset
    $sectionSize = Read-U32BE $Bytes ($sectionOffset + 4)
    $sectionItemCount = Read-U32BE $Bytes ($sectionOffset + 8)
    $sectionName = Get-G1MGSectionName $sectionMagic
    $nextSectionOffset = $sectionOffset + [int]$sectionSize
    $notes = ("section_index={0}; id=0x{1:X8}; count={2}" -f $i, $sectionMagic, $sectionItemCount)

    if ($sectionSize -lt 12 -or $nextSectionOffset -gt $chunkEnd -or $nextSectionOffset -gt $Bytes.Length) {
      $Rows.Add([pscustomobject]@{
        File = $Path
        Offset = ("0x{0:X8}" -f $sectionOffset)
        Magic = "G1MG.$sectionName"
        Version = ""
        Size = $sectionSize
        NextOffset = ""
        Notes = "$notes; invalid section size"
      })
      break
    }

    $Rows.Add([pscustomobject]@{
      File = $Path
      Offset = ("0x{0:X8}" -f $sectionOffset)
      Magic = "G1MG.$sectionName"
      Version = ""
      Size = $sectionSize
      NextOffset = ("0x{0:X8}" -f $nextSectionOffset)
      Notes = $notes
    })

    switch ($sectionMagic) {
      0x00010005 {
        Add-G1MGVertexAttributeRows $Rows $Bytes $Path $sectionOffset ([int]$sectionSize) ([int]$sectionItemCount)
      }
      0x00010007 {
        Add-G1MGIndexBufferRows $Rows $Bytes $Path $sectionOffset ([int]$sectionSize) ([int]$sectionItemCount) $G1MGVersion
      }
      0x00010008 {
        Add-G1MGSubmeshRows $Rows $Bytes $Path $sectionOffset ([int]$sectionSize) ([int]$sectionItemCount)
      }
    }

    $sectionOffset = $nextSectionOffset
  }
}

function Scan-G1MFile([string]$Path) {
  $bytes = [IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -lt 0x18) {
    throw "G1M file is too small: $Path"
  }

  $magic = Read-Ascii $bytes 0 4
  if ($magic -ne "G1M_") {
    throw "Input is not a G1M_ file: $Path"
  }

  $version = Read-Ascii $bytes 4 4
  $declaredPayloadSize = Read-U32BE $bytes 8
  $firstChunkOffset = Read-U32BE $bytes 12
  $unknown10 = Read-U32BE $bytes 16
  $chunkCountHint = Read-U32BE $bytes 20

  $rows = New-Object System.Collections.Generic.List[object]
  $rows.Add([pscustomobject]@{
    File = $Path
    Offset = "0x00000000"
    Magic = $magic
    Version = $version
    Size = $declaredPayloadSize
    NextOffset = ("0x{0:X8}" -f $firstChunkOffset)
    Notes = ("top payload size; unknown10=0x{0:X8}; chunk_count_hint={1}" -f $unknown10, $chunkCountHint)
  })

  $cursor = [int]$firstChunkOffset
  $guard = 0
  while ($cursor -ge 0 -and $cursor + 12 -le $bytes.Length -and $guard -lt 512) {
    $chunkMagic = Read-Ascii $bytes $cursor 4
    if ($chunkMagic -notmatch '^G1M[A-Z_]$') {
      break
    }

    $chunkVersion = Read-Ascii $bytes ($cursor + 4) 4
    $chunkSize = Read-U32BE $bytes ($cursor + 8)
    if ($chunkSize -lt 12 -or $cursor + $chunkSize -gt $bytes.Length) {
      $rows.Add([pscustomobject]@{
        File = $Path
        Offset = ("0x{0:X8}" -f $cursor)
        Magic = $chunkMagic
        Version = $chunkVersion
        Size = $chunkSize
        NextOffset = ""
        Notes = "invalid chunk size"
      })
      break
    }

    $next = $cursor + [int]$chunkSize
    $rows.Add([pscustomobject]@{
      File = $Path
      Offset = ("0x{0:X8}" -f $cursor)
      Magic = $chunkMagic
      Version = $chunkVersion
      Size = $chunkSize
      NextOffset = ("0x{0:X8}" -f $next)
      Notes = ""
    })

    if ($chunkMagic -eq "G1MG") {
      Add-G1MGSectionRows $rows $bytes $Path $cursor ([int]$chunkSize) $chunkVersion
    }

    $cursor = $next
    $guard++
  }

  if ($cursor -lt $bytes.Length) {
    $rows.Add([pscustomobject]@{
      File = $Path
      Offset = ("0x{0:X8}" -f $cursor)
      Magic = ""
      Version = ""
      Size = $bytes.Length - $cursor
      NextOffset = ("0x{0:X8}" -f $bytes.Length)
      Notes = "trailing or unparsed bytes"
    })
  }

  return $rows
}

$resolved = Resolve-Path -LiteralPath $InputPath
$results = New-Object System.Collections.Generic.List[object]
if ((Get-Item -LiteralPath $resolved).PSIsContainer) {
  foreach ($file in Get-ChildItem -LiteralPath $resolved -Recurse -File -Filter "*.g1m") {
    foreach ($row in Scan-G1MFile $file.FullName) {
      $results.Add($row)
    }
  }
} else {
  foreach ($row in Scan-G1MFile $resolved.Path) {
    $results.Add($row)
  }
}

if ($OutputPath) {
  $parent = Split-Path -Parent $OutputPath
  if ($parent -and !(Test-Path -LiteralPath $parent)) {
    [void][IO.Directory]::CreateDirectory($parent)
  }
  $results | Export-Csv -LiteralPath $OutputPath -NoTypeInformation
}

$results
