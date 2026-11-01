param([Parameter(Mandatory=$true)][string]$Archive,
      [Parameter(Mandatory=$true)][string]$Destination,
      [Parameter(Mandatory=$true)][string]$Prefix)
$ErrorActionPreference = 'Stop'
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $Destination) { throw 'Destination already exists' }
    [IO.Directory]::CreateDirectory($Destination) | Out-Null
    $root = [IO.Path]::GetFullPath($Destination).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $archiveFile = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        [long]$total = 0
        if ($archiveFile.Entries.Count -gt 20000) { throw 'Too many entries' }
        foreach ($entry in $archiveFile.Entries) {
            $name = $entry.FullName
            if (-not $name.StartsWith($Prefix + '/', [StringComparison]::Ordinal)) { throw 'Unexpected archive root' }
            $name = $name.Substring($Prefix.Length + 1)
            if ($name.Length -eq 0) { continue }
            if ($name.Contains('\') -or $name.Contains(':') -or $name.StartsWith('/')) { throw 'Invalid path' }
            $directory = $name.EndsWith('/')
            $relative = $name.TrimEnd('/')
            if (-not $seen.Add($relative)) { throw 'Duplicate archive path' }
            foreach ($part in $relative.Split('/')) {
                if ($part -eq '' -or $part -eq '.' -or $part -eq '..' -or $part -match '[<>"|?*\x00-\x1f]' -or
                    $part.EndsWith('.') -or $part.EndsWith(' ') -or $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') { throw 'Unsafe path segment' }
            }
            $mode = ($entry.ExternalAttributes -shr 16) -band 0xf000
            if ($mode -eq 0xa000 -or ($mode -ne 0 -and $mode -ne 0x8000 -and $mode -ne 0x4000) -or
                ($entry.ExternalAttributes -band 0x400) -ne 0) { throw 'Links and special files are not allowed' }
            $total += $entry.Length
            if ($entry.Length -gt 1GB -or $total -gt 2GB -or
                ($entry.Length -gt 1MB -and $entry.Length / [Math]::Max(1,$entry.CompressedLength) -gt 500)) { throw 'Archive size limit' }
            $target = [IO.Path]::GetFullPath([IO.Path]::Combine($root, $relative.Replace('/', [IO.Path]::DirectorySeparatorChar)))
            if (-not $target.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw 'Path escapes destination' }
            if ($directory) { [IO.Directory]::CreateDirectory($target) | Out-Null; continue }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
            $inputStream = $entry.Open()
            $outputStream = New-Object IO.FileStream($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try {
                $buffer = New-Object byte[] 1048576
                [long]$written = 0
                while (($count = $inputStream.Read($buffer,0,$buffer.Length)) -gt 0) {
                    $written += $count
                    if ($written -gt $entry.Length) { throw 'Invalid expanded size' }
                    $outputStream.Write($buffer,0,$count)
                }
                if ($written -ne $entry.Length) { throw 'Incomplete entry' }
            } finally { $outputStream.Dispose(); $inputStream.Dispose() }
        }
    } finally { $archiveFile.Dispose() }
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
