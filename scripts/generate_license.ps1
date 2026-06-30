param (
    [string]$ExpiryDate = (Get-Date).AddYears(1).ToString("yyyy-MM-dd")
)

$privateKeyPath = Join-Path $PSScriptRoot "private_key.xml"

if (-not (Test-Path $privateKeyPath)) {
    Write-Host "[INFO] Generating new RSA 2048 key pair..."
    $rsaGen = [System.Security.Cryptography.RSACryptoServiceProvider]::new(2048)
    $privateXml = $rsaGen.ToXmlString($true)
    Set-Content -Path $privateKeyPath -Value $privateXml -Encoding UTF8
    
    $pubBlob = $rsaGen.ExportCspBlob($false)
    $pubBlobHex = [BitConverter]::ToString($pubBlob) -replace '-'
    Write-Host ""
    Write-Host "!!! NEW PUBLIC KEY GENERATED !!!"
    Write-Host "Please update the public key blob in src/core/main.cpp with the following:"
    Write-Host $pubBlobHex
    Write-Host "--------------------------------"
    Write-Host ""
}

$dateStr = [datetime]::Parse($ExpiryDate).ToString("yyyyMMdd")

$rsa = [System.Security.Cryptography.RSACryptoServiceProvider]::new()
$privateXml = Get-Content $privateKeyPath -Raw
$rsa.FromXmlString($privateXml)

$dataToSign = [System.Text.Encoding]::UTF8.GetBytes($dateStr)
$signatureBytes = $rsa.SignData($dataToSign, [System.Security.Cryptography.SHA256CryptoServiceProvider]::new())
$signatureBase64 = [Convert]::ToBase64String($signatureBytes)

$licenseKey = "${dateStr}-${signatureBase64}"

Write-Host "============================="
Write-Host " TELOPHUB LICENSE GENERATOR "
Write-Host "============================="
Write-Host "Expiry Date: $ExpiryDate"
Write-Host "License Key: $licenseKey" -ForegroundColor Green
Write-Host "============================="
Write-Host ""
Write-Host "Put the above key in licensekey.json like this:"
Write-Host "{"
Write-Host "  `"license_key`": `"$licenseKey`""
Write-Host "}"
