<#
.SYNOPSIS
    Builds and signs the image manifest for the secure bootloader.

.DESCRIPTION
    Reads an application .bin, builds the 128-byte manifest (protocol spec section 7),
    signs manifest bytes 0..63 with ECDSA P-256 / SHA-256 using OpenSSL and writes a
    signed package:

        bytes 0..127   signed manifest  -> send unchanged in UPDATE_BEGIN
        bytes 128..    raw image        -> send in UPDATE_DATA blocks

    The signature is checked again with .NET before the file is written, against the
    public key of the given private key. The script prints that public key so it can
    be compared with bl_public_key.c.

.EXAMPLE
    .\sign_image.ps1 -Image ..\..\UserApp\Debug\UserApp.bin -Key ..\..\Keys\dev_signing_key.pem -Version 1.0.0
#>
param(
    [Parameter(Mandatory = $true)] [string] $Image,
    [Parameter(Mandatory = $true)] [string] $Key,
    [string] $Version = "1.0.0",            # major.minor.patch, informational
    [uint32] $SecurityCounter = 0,          # anti-rollback counter, raise only for security fixes
    [uint16] $HwId = 0x0001,                # must match BL_HW_ID
    [uint32] $LoadAddress = 0x08008000,     # must match BL_APP_START
    [string] $Out = "",                     # default: <image>_signed.bin next to the image
    [string] $OpenSsl = "C:\Program Files\Git\usr\bin\openssl.exe"
)

$ErrorActionPreference = "Stop"

$MANIFEST_MAGIC = [uint32]0x464D4C42
$FLAG_SIGNED    = [uint32]1
$APP_MAX_SIZE   = 229376

if (-not (Test-Path $OpenSsl)) {
    $cmd = Get-Command openssl -ErrorAction SilentlyContinue
    if ($null -eq $cmd) { throw "openssl not found, pass -OpenSsl <path>" }
    $OpenSsl = $cmd.Source
}

if (-not ('BlCrc32' -as [type])) {
    Add-Type -TypeDefinition @"
public static class BlCrc32 {
    // CRC-32/ISO-HDLC, same as zlib / binascii.crc32
    public static uint Compute(byte[] d) {
        uint c = 0xFFFFFFFF;
        foreach (byte b in d) {
            c ^= b;
            for (int k = 0; k < 8; k++)
                c = (c & 1) != 0 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        }
        return ~c;
    }
}
"@
}

# Strip a DER INTEGER to exactly 32 big-endian bytes
function ConvertTo-Fixed32([byte[]] $v) {
    $l = New-Object System.Collections.Generic.List[byte]
    $l.AddRange($v)
    while ($l.Count -gt 32 -and $l[0] -eq 0) { $l.RemoveAt(0) }
    if ($l.Count -gt 32) { throw "signature integer longer than 32 bytes" }
    while ($l.Count -lt 32) { $l.Insert(0, 0) }
    return ,$l.ToArray()
}

# DER ECDSA-Sig-Value (SEQUENCE { INTEGER r, INTEGER s }) -> r || s
function ConvertFrom-DerSignature([byte[]] $der) {
    if ($der[0] -ne 0x30 -or ($der[1] -band 0x80)) { throw "unexpected DER signature format" }
    $i = 2
    if ($der[$i] -ne 0x02) { throw "DER: r is not an INTEGER" }
    $lr = $der[$i + 1]; $r = ConvertTo-Fixed32 $der[($i + 2)..($i + 1 + $lr)]; $i += 2 + $lr
    if ($der[$i] -ne 0x02) { throw "DER: s is not an INTEGER" }
    $ls = $der[$i + 1]; $s = ConvertTo-Fixed32 $der[($i + 2)..($i + 1 + $ls)]
    return ,([byte[]]($r + $s))
}

# --- Image ---------------------------------------------------------------
$imagePath = (Resolve-Path $Image).Path
$keyPath   = (Resolve-Path $Key).Path
$img = [IO.File]::ReadAllBytes($imagePath)
if ($img.Length -eq 0 -or $img.Length -gt $APP_MAX_SIZE) { throw "image size $($img.Length) out of range (1..$APP_MAX_SIZE)" }

$v = $Version.Split(".")
if ($v.Count -ne 3) { throw "version must be major.minor.patch" }
$imageVersion = ([uint32]$v[0] -shl 24) -bor ([uint32]$v[1] -shl 16) -bor [uint32]$v[2]

$crc = [BlCrc32]::Compute($img)
$sha = [Security.Cryptography.SHA256]::Create().ComputeHash($img)

# --- Manifest bytes 0..63 (little-endian) --------------------------------
$ms = New-Object IO.MemoryStream
$w  = New-Object IO.BinaryWriter($ms)
$w.Write([uint32]$MANIFEST_MAGIC)
$w.Write([uint16]1)                  # manifest_version
$w.Write([uint16]$HwId)
$w.Write([uint32]$FLAG_SIGNED)
$w.Write([uint32]$imageVersion)
$w.Write([uint32]$SecurityCounter)
$w.Write([uint32]$LoadAddress)
$w.Write([uint32]$img.Length)
$w.Write([uint32]$crc)
$w.Write($sha)
$w.Flush()
$m64 = $ms.ToArray()
if ($m64.Length -ne 64) { throw "internal: manifest header is $($m64.Length) bytes" }

# --- Sign with OpenSSL ---------------------------------------------------
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("bl_sign_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $tmp | Out-Null
try {
    [IO.File]::WriteAllBytes("$tmp\m64.bin", $m64)
    & $OpenSsl dgst -sha256 -sign $keyPath -out "$tmp\sig.der" "$tmp\m64.bin"
    if ($LASTEXITCODE -ne 0) { throw "openssl signing failed" }
    # Not redirected: in Windows PowerShell a redirected stderr line ("read EC key") becomes an error
    & $OpenSsl ec -in $keyPath -pubout -outform DER -out "$tmp\pub.der"
    if ($LASTEXITCODE -ne 0) { throw "openssl could not read the public key" }
    $sig = ConvertFrom-DerSignature ([IO.File]::ReadAllBytes("$tmp\sig.der"))
    $pubDer = [IO.File]::ReadAllBytes("$tmp\pub.der")
} finally {
    Remove-Item -Recurse -Force $tmp
}
$pub = [byte[]]$pubDer[($pubDer.Length - 64)..($pubDer.Length - 1)]

# --- Independent check of the raw r||s signature with .NET ---------------
$pt = New-Object Security.Cryptography.ECPoint
$pt.X = [byte[]]$pub[0..31]
$pt.Y = [byte[]]$pub[32..63]
$prm = New-Object Security.Cryptography.ECParameters
$prm.Curve = [Security.Cryptography.ECCurve+NamedCurves]::nistP256
$prm.Q = $pt
$ec = [Security.Cryptography.ECDsa]::Create($prm)
if (-not $ec.VerifyData($m64, $sig, [Security.Cryptography.HashAlgorithmName]::SHA256)) {
    throw "signature self-check failed, nothing written"
}

# --- Write package -------------------------------------------------------
if ($Out -eq "") {
    $Out = Join-Path (Split-Path $imagePath) ([IO.Path]::GetFileNameWithoutExtension($imagePath) + "_signed.bin")
}
$pkg = New-Object byte[] (128 + $img.Length)
[Array]::Copy($m64, 0, $pkg, 0, 64)
[Array]::Copy($sig, 0, $pkg, 64, 64)
[Array]::Copy($img, 0, $pkg, 128, $img.Length)
[IO.File]::WriteAllBytes($Out, $pkg)

function Hex([byte[]] $b) { ($b | ForEach-Object { $_.ToString("x2") }) -join "" }
"Image            : $imagePath ($($img.Length) bytes)"
"Version          : $Version (0x{0:X8})" -f $imageVersion
"Security counter : $SecurityCounter"
"hw_id            : 0x{0:X4}" -f $HwId
"Load address     : 0x{0:X8}" -f $LoadAddress
"CRC-32           : 0x{0:X8}" -f $crc
"SHA-256          : $(Hex $sha)"
"Public key X||Y  : $(Hex $pub)"
"Signature        : verified"
"Package          : $Out ($($pkg.Length) bytes = 128-byte manifest + image)"
