# Teste de integracao do app.c desta edicao contra o servidor EPTX.
#
# Sobe o codigo do servidor do EPTX Live (D:\EPTX Live\src, so lido) numa instancia
# de teste: porta 18360, banco e biblioteca em tests\.work. O .env, o banco e a
# biblioteca do EPTX Live nao sao usados nem tocados.
#
#   powershell -ExecutionPolicy Bypass -File tests\run-integration.ps1
param(
	[string]$Live = 'D:\EPTX Live',
	[int]$Port = 18360
)
$ErrorActionPreference = 'Stop'
$edition = Split-Path -Parent $PSScriptRoot
$work = Join-Path $PSScriptRoot '.work'
$xbox = Join-Path $edition 'client\xbox'
$core = Join-Path $edition 'client\core'

if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force "$work\data", "$work\library\Homebrew Teste", "$work\library\Outro Jogo" | Out-Null
# Dois titulos para a loja: pastas homebrew com default.xex.
[IO.File]::WriteAllBytes("$work\library\Homebrew Teste\default.xex", [byte[]]::new(4096))
[IO.File]::WriteAllBytes("$work\library\Outro Jogo\default.xex", [byte[]]::new(4096))

Write-Host '== compilando app_test.exe (app.c em modo PC, com zig)'
$sources = @("$PSScriptRoot\app_test.c", "$xbox\app.c", "$xbox\image.c", "$core\eptx_client.c", "$core\eptx_http.c",
	"$core\eptx_json.c", "$core\eptx_sha256.c", "$core\eptx_sign.c")
& zig cc -target x86_64-windows-gnu -O1 -DEPTX_PC -I"$xbox" -I"$core" -I"$xbox\third_party\stb" -I"$PSScriptRoot\stub" `
	@sources -lws2_32 -o "$work\app_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'compilacao falhou' }

Write-Host "== servidor de teste na porta $Port"
$env:EPTX_PORT = "$Port"
$env:EPTX_HOST = '127.0.0.1'
$env:EPTX_DB = "$work\data\eptx.db"
$env:EPTX_LIBRARY = "$work\library"
$env:EPTX_SECRET = [guid]::NewGuid().ToString('N') + [guid]::NewGuid().ToString('N')
$env:EPTX_LOG_LEVEL = 'warn'
$server = Start-Process node -ArgumentList "`"$Live\src\index.ts`"" -WorkingDirectory $work -PassThru -NoNewWindow `
	-RedirectStandardOutput "$work\server.log" -RedirectStandardError "$work\server.err"
$failed = 0
try {
	$base = "http://127.0.0.1:$Port"
	for ($i = 0; $i -lt 50; $i++) {
		try { Invoke-RestMethod "$base/health" | Out-Null; break } catch { Start-Sleep -Milliseconds 200 }
	}
	foreach ($tag in 'alice', 'bob', 'carol') {
		$body = @{ gamertag = $tag; password = "senha-$tag-123" } | ConvertTo-Json
		Invoke-RestMethod -Method Post "$base/api/v1/auth/register" -ContentType 'application/json' -Body $body | Out-Null
		New-Item -ItemType Directory -Force "$work\$tag" | Out-Null
		Set-Content "$work\$tag\eptx.ini" "host=127.0.0.1`nport=$Port`nuser=$tag`npass=senha-$tag-123" -Encoding ascii
	}
	# Perfis do Xbox de mentira no "HD" de PC de alice e bob: o pacote STFS so com o que a dash le,
	# a gamertag (UTF-16BE em 0x411) e a foto (tamanho em 0x1712, PNG em 0x171A). carol fica sem
	# pacote: a dash usa a gamertag como chave e nao manda foto.
	$skinImages = Join-Path $edition 'freestyledash\Skins\Default\Images\Interface\Dash Artwork'
	$pics = @{ alice = "$skinImages\GScore.png"; bob = "$skinImages\ico_64x_HD.png" }
	$xuids = @{ alice = 'E000000000000A11'; bob = 'E000000000000B0B' }
	foreach ($tag in 'alice', 'bob') {
		$png = [IO.File]::ReadAllBytes($pics[$tag])
		$pkg = [byte[]]::new(0x171A + $png.Length)
		[Text.Encoding]::ASCII.GetBytes('CON ').CopyTo($pkg, 0)
		[Text.Encoding]::BigEndianUnicode.GetBytes($tag).CopyTo($pkg, 0x411)
		$size = [BitConverter]::GetBytes([uint32]$png.Length); [Array]::Reverse($size); $size.CopyTo($pkg, 0x1712)
		$png.CopyTo($pkg, 0x171A)
		$dir = "$work\$tag\pc-hdd\Content\$($xuids[$tag])\FFFE07D1\00010000"
		New-Item -ItemType Directory -Force $dir | Out-Null
		[IO.File]::WriteAllBytes("$dir\$($xuids[$tag])", $pkg)
	}
	# O passo da sessao expirada apaga as sessoes do banco de teste, como o servidor faria ao revogar.
	$env:EXPIRE_CMD = "node -e `"new (require('node:sqlite').DatabaseSync)(process.argv[1]).exec('DELETE FROM sessions')`" `"$work\data\eptx.db`""

	$steps = @(('alice', 'alice-send'), ('bob', 'bob-accept'), ('carol', 'carol-decline'), ('alice', 'alice-room'),
		('bob', 'bob-join'), ('alice', 'alice-cleanup'), ('alice', 'alice-profiles'), ('alice', 'alice-expire'))
	foreach ($s in $steps) {
		$dir = "$work\$($s[0])"
		$p = Start-Process "$work\app_test.exe" -ArgumentList $s[1] -WorkingDirectory $dir -NoNewWindow -PassThru `
			-RedirectStandardOutput "$dir\$($s[1]).out" -RedirectStandardError "$dir\$($s[1]).log"
		$null = $p.Handle # sem isto o ExitCode volta vazio no PowerShell 5
		if (-not $p.WaitForExit(60000)) {
			Stop-Process -Id $p.Id -Force
			Write-Host "FALHOU  $($s[1]) passou de 60 s"
			$failed++
		} elseif ($p.ExitCode -ne 0) {
			$failed++
		}
		Get-Content "$dir\$($s[1]).out" | Write-Host
	}

	# A foto de cada perfil chegou ao servidor e e o avatar que os amigos veem; carol, sem pacote, nao mandou.
	Write-Host '== avatares no servidor'
	foreach ($tag in 'alice', 'bob', 'carol') {
		$file = "$work\avatar-$tag.png"
		Invoke-WebRequest "$base/avatar/$tag/avatar-body.png" -OutFile $file -UseBasicParsing
		$served = (Get-FileHash $file).Hash
		$same = $pics.ContainsKey($tag) -and $served -eq (Get-FileHash $pics[$tag]).Hash
		$ok = if ($pics.ContainsKey($tag)) { $same } else { -not $same }
		Write-Host ("{0}  {1}: {2}" -f $(if ($ok) { 'ok    ' } else { 'FALHOU' }), $tag,
			$(if ($pics.ContainsKey($tag)) { 'avatar-body.png e a foto do perfil' } else { 'sem perfil, avatar continua o da conta' }))
		if (-not $ok) { $failed++ }
	}
} finally {
	Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "== $failed passo(s) com falha"; exit 1 }
Write-Host '== integracao: todos os passos passaram'
