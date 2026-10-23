# Synthetic staging tests. Signature checks are stubbed for harmless fixture
# files; these tests do not establish real signing or installer acceptance.
$ErrorActionPreference = 'Stop'
$package = Join-Path $PSScriptRoot '..\scripts\package_windows_release.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ('LoopZ-payload-test-' + [Guid]::NewGuid().ToString('N'))
function Require([bool]$condition, [string]$message) {
    if (!$condition) { throw $message }
}
function Get-AuthenticodeSignature {
    param([string]$LiteralPath)
    $contents=Get-Content -LiteralPath $LiteralPath -Raw
    $status=if($contents.StartsWith('untrusted-')){'NotTrusted'}else{'Valid'}
    $subject = if ($contents -in @('runtime','untrusted-runtime')) {
        'CN=Microsoft Corporation, O=Microsoft Corporation'
    } elseif ($contents -in @('microsoft','untrusted-microsoft')) {
        'CN=Microsoft Windows Hardware Compatibility Publisher'
    } else { 'CN=LoopZ development fixture' }
    [PSCustomObject]@{Status=$status; SignerCertificate=[PSCustomObject]@{Subject=$subject}}
}
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $app = New-Item -ItemType Directory -Path (Join-Path $root 'app')
    $driver = New-Item -ItemType Directory -Path (Join-Path $root 'signed-driver')
    $oldDriver = New-Item -ItemType Directory -Path (Join-Path $app.FullName 'driver')
    foreach ($name in @('LoopZ.exe','loopz-driver-setup.exe','loopz-windows-control.exe')) {
        [IO.File]::WriteAllText((Join-Path $app.FullName $name), 'application fixture')
    }
    [IO.File]::WriteAllText((Join-Path $oldDriver.FullName 'DEVELOPMENT-ONLY.txt'), 'stale driver')
    [IO.File]::WriteAllText((Join-Path $app.FullName 'manifest.json'), 'stale manifest')
    foreach ($name in @('LoopZAudio.inf','LoopZAudio.sys','LoopZAudio.cat')) {
        [IO.File]::WriteAllText((Join-Path $driver.FullName $name), 'microsoft')
    }
    $signTool = Join-Path $root 'sign-tool-fixture.cmd'
    $signToolCode=@'
@echo off
if /i "%~2"=="/pa" if exist "%~dp0reject-%~nx3" exit /b 1
if /i "%~2"=="/kp" if exist "%~dp0reject-%~nx5" exit /b 1
exit /b 0
'@
    [IO.File]::WriteAllText($signTool, $signToolCode.Replace("`r`n","`n").Replace("`n","`r`n"))
    $runtime = Join-Path $root 'WebView2RuntimeInstaller.exe'
    [IO.File]::WriteAllText($runtime, 'runtime')
    $stalePrerequisite = New-Item -ItemType Directory -Path (Join-Path $app.FullName 'prerequisites')
    [IO.File]::WriteAllText((Join-Path $stalePrerequisite.FullName 'stale.exe'), 'stale runtime')
    $output = Join-Path $root 'output'
    & $package -AppDirectory $app.FullName -SignedDriverDirectory $driver.FullName -WebView2Installer $runtime -OutputDirectory $output -SignTool $signTool
    Require (!(Test-Path (Join-Path $output 'driver\DEVELOPMENT-ONLY.txt'))) 'Stale development driver file was packaged'
    $manifest = Get-Content (Join-Path $output 'manifest.json') -Raw | ConvertFrom-Json
    Require (!($manifest | Where-Object path -eq 'manifest.json')) 'Manifest contains its own stale hash'
    $files = @(Get-ChildItem $output -Recurse -File | Where-Object Name -ne 'manifest.json')
    Require ($manifest.Count -eq $files.Count) ("Manifest file coverage differs: entries="+$manifest.Count+", files="+$files.Count)
    foreach ($entry in $manifest) {
        Require ((Get-FileHash (Join-Path $output $entry.path)).Hash.ToLowerInvariant() -eq $entry.sha256) 'Manifest hash mismatch'
    }
    foreach ($name in @('LoopZAudio.inf','LoopZAudio.sys','LoopZAudio.cat')) {
        Require ((Get-FileHash (Join-Path $output ('driver\'+$name))).Hash -eq (Get-FileHash (Join-Path $driver.FullName $name)).Hash) 'Explicit driver payload differs'
    }
    Require (!(Test-Path (Join-Path $output 'prerequisites\stale.exe'))) 'Stale prerequisite was packaged'
    Require ((Get-FileHash (Join-Path $output 'prerequisites\WebView2RuntimeInstaller.exe')).Hash -eq (Get-FileHash $runtime).Hash) 'Explicit WebView2 installer differs'
    Require (Test-Path (Join-Path $oldDriver.FullName 'DEVELOPMENT-ONLY.txt')) 'Input driver was modified'
    $rejectedOutput = Join-Path $root 'rejected-output'
    [IO.File]::WriteAllText((Join-Path $driver.FullName 'LoopZAudio.cat'), 'development')
    $rejected = $false
    try { & $package -AppDirectory $app.FullName -SignedDriverDirectory $driver.FullName -WebView2Installer $runtime -OutputDirectory $rejectedOutput -SignTool $signTool }
    catch { $rejected = $true }
    Require ($rejected -and !(Test-Path $rejectedOutput)) 'Development catalog signer was admitted'
    $rejected = $false
    try { & $package -AppDirectory $app.FullName -SignedDriverDirectory $driver.FullName -WebView2Installer $runtime -OutputDirectory $output -SignTool $signTool }
    catch { $rejected = $true }
    Require $rejected 'Existing output was overwritten'
    [IO.File]::WriteAllText((Join-Path $driver.FullName 'LoopZAudio.cat'), 'microsoft')
    [IO.File]::WriteAllText($runtime, 'development')
    $runtimeOutput = Join-Path $root 'rejected-runtime'
    $rejected = $false
    try { & $package -AppDirectory $app.FullName -SignedDriverDirectory $driver.FullName -WebView2Installer $runtime -OutputDirectory $runtimeOutput -SignTool $signTool }
    catch { $rejected = $true }
    Require ($rejected -and !(Test-Path $runtimeOutput)) 'Non-Microsoft runtime installer was admitted'
    [IO.File]::WriteAllText($runtime, 'runtime')
    $failures=@(
        @{name='runtime-trust';path=$runtime;content='untrusted-runtime';error='valid Microsoft-signed'},
        @{name='catalog-trust';path=(Join-Path $driver.FullName 'LoopZAudio.cat');content='untrusted-microsoft';error='Microsoft Hardware Dashboard'},
        @{name='runtime-policy';file='WebView2RuntimeInstaller.exe';error='WebView2 installer signature verification failed'},
        @{name='application-policy';file='LoopZ.exe';error='Invalid application signature'},
        @{name='inf-membership';file='LoopZAudio.inf';error='Driver signature or catalog membership invalid'},
        @{name='sys-membership';file='LoopZAudio.sys';error='Driver signature or catalog membership invalid'}
    )
    foreach($case in $failures) {
        $caseOutput=Join-Path $root ('rejected-'+$case.name)
        $marker=$null
        $original=$null
        if($case.path) {
            $original=[IO.File]::ReadAllText($case.path)
            [IO.File]::WriteAllText($case.path,$case.content)
        } else {
            $marker=Join-Path $root ('reject-'+$case.file)
            [IO.File]::WriteAllText($marker,'reject this signature check')
        }
        try {
            $message=''
            try { & $package -AppDirectory $app.FullName -SignedDriverDirectory $driver.FullName -WebView2Installer $runtime -OutputDirectory $caseOutput -SignTool $signTool }
            catch { $message=$_.Exception.Message }
            Require ($message -match $case.error -and !(Test-Path $caseOutput)) ('Packaging failure gate did not reject before staging: '+$case.name+'; '+$message)
        } finally {
            if($marker){Remove-Item -LiteralPath $marker}
            if($case.path){[IO.File]::WriteAllText($case.path,$original)}
        }
    }
    @{ok=$true; syntheticSignatures=$true; realSigningAcceptance=$false; cases=(4+$failures.Count)} | ConvertTo-Json
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
