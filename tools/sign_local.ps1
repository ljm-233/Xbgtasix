<#
sign_local.ps1 —— 给 xbgtasix.exe 做本机自签名，去掉"发布者未知"的 SmartScreen 提示

原理（先看清局限，别误解）：
  SmartScreen 拦的是"未签名且没有信誉积累"的程序。自签名证书能解决"发布者未知"，
  但**只在你把这台机器设为信任该证书后有效**。给别人用仍会提示——那需要真正的
  代码签名证书（EV 证书）+ 信誉积累，属于花钱的事，不是脚本能解决的。

做了什么：
  1. 在 CurrentUser 的"我的证书"里建一张自签名 Code Signing 证书（不需要管理员）
  2. 把它装进 CurrentUser 的「受信任的根证书颁发机构」和「受信任的发布者」
     （只影响当前用户，不动系统级信任库）
  3. 用它对 build\xbgtasix.exe 签名，并校验签名

用法：
  powershell -ExecutionPolicy Bypass -File tools\sign_local.ps1
  powershell -ExecutionPolicy Bypass -File tools\sign_local.ps1 -Exe "build\xbgtasix.exe"
  powershell -ExecutionPolicy Bypass -File tools\sign_local.ps1 -Remove   # 撤销信任并删证书
#>
param(
    [string]$Exe = "build\xbgtasix.exe",
    [string]$Subject = "CN=Xbgtasix Local Dev",
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path $repo $Exe }

function Get-OurCerts {
    Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -eq $Subject }
}

if ($Remove) {
    foreach ($c in Get-OurCerts) {
        foreach ($store in @("Root", "TrustedPublisher", "My")) {
            $path = "Cert:\CurrentUser\$store"
            Get-ChildItem $path | Where-Object { $_.Thumbprint -eq $c.Thumbprint } |
                ForEach-Object { Remove-Item $_.PSPath -Force -ErrorAction SilentlyContinue }
        }
        Write-Host "已移除证书 $($c.Thumbprint)"
    }
    exit 0
}

Write-Host "== 1) 准备自签名证书"
$cert = Get-OurCerts | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -KeyUsage DigitalSignature `
        -KeyExportPolicy Exportable `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -NotAfter (Get-Date).AddYears(5) `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")   # EKU: Code Signing
    Write-Host "  已新建证书 $($cert.Thumbprint)"
} else {
    Write-Host "  复用已有证书 $($cert.Thumbprint)"
}

Write-Host "== 2) 装进信任库（仅当前用户）"
foreach ($store in @("Root", "TrustedPublisher")) {
    $path = "Cert:\CurrentUser\$store"
    $exists = Get-ChildItem $path | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
    if ($exists) {
        Write-Host "  已在 $store"
    } else {
        $tmp = Join-Path $env:TEMP "xbgtasix_sign.cer"
        Export-Certificate -Cert $cert -FilePath $tmp -Force | Out-Null
        Import-Certificate -FilePath $tmp -CertStoreLocation $path | Out-Null
        Remove-Item $tmp -Force
        Write-Host "  已加入 $store"
    }
}

Write-Host "== 3) 签名 $Exe"
if (-not (Test-Path $Exe)) { throw "找不到 $Exe（先运行 build.sh）" }
$sig = Set-AuthenticodeSignature -FilePath $Exe -Certificate $cert -HashAlgorithm SHA256
if ($sig.Status -ne "Valid") {
    Write-Host "  签名状态：$($sig.Status) —— $($sig.StatusMessage)" -ForegroundColor Yellow
} else {
    Write-Host "  签名成功（SHA256）"
}

Write-Host "== 4) 校验"
$check = Get-AuthenticodeSignature $Exe
Write-Host "  文件：$Exe"
Write-Host "  状态：$($check.Status)"
if ($check.SignerCertificate) {
    Write-Host "  主体：$($check.SignerCertificate.Subject)"
    Write-Host "  指纹：$($check.SignerCertificate.Thumbprint)"
}
if ($check.Status -eq "Valid") {
    Write-Host "`n完成：在这台机器上运行时不会再提示「发布者未知」。" -ForegroundColor Green
    Write-Host "注意：换一台机器仍需重新信任该证书；对外分发需要真正的代码签名证书。"
} else {
    Write-Host "`n签名已写入，但校验未通过（见上面的状态）。可以先把文件解除锁定再试。" -ForegroundColor Yellow
}
