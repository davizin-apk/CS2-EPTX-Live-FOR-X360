# EPTX Live

Serviço self-hosted para Xbox 360 destravado (RGH/JTAG). **No console você nunca sai
da dash original.** Nada de homebrew na tela: você escolhe o jogo no celular ou no
PC, o servidor grava direto no HD do console, e o jogo aparece em "Meus Jogos" na
NXE original — com capa, e rodando de lá.

Não fala com a Microsoft. Não implementa o protocolo do Xbox LIVE. Não faz
multiplayer online.

---

## Onde está a linha

Vale ser exato sobre o que dá e o que não dá, porque não é a mesma resposta para
cada parte.

### Jogos na dash original — funciona

Um pacote GOD em `Hdd1:\Content\0000000000000000\<TitleID>\00007000\` aparece
sozinho em "Meus Jogos" da NXE, com a capa que vem no próprio cabeçalho do pacote, e
inicia dali. No RGH o freeboot não checa assinatura de conteúdo, então não precisa de
licença nem de nada assinado pela Microsoft. Isso é nativo. O problema nunca foi o
jogo — foi **como ele chega no HD**, e é exatamente esse o buraco que este projeto
tapa.

### Amigos e chat na dash original — não funciona

Não é "difícil": é bloqueado. As telas de amigos, mensagens e chat da NXE só se
populam a partir de uma sessão LIVE autenticada, e essa autenticação usa a chave
privada única do keyvault do console mais certificados assinados pela Microsoft,
validados contra raízes gravadas dentro do `xam.xex`.

Seu servidor pode **aceitar** qualquer coisa que o console mandar — você é o
servidor. O problema é o contrário: você não consegue **assinar as respostas** que o
console valida, porque não tem a chave privada da Microsoft. Dá para patchear o
`xam.xex` num RGH e desligar essa validação, mas aí cada serviço fala protocolo
binário proprietário e as capturas da época estão criptografadas. Ninguém completou
esse trabalho publicamente para o 360. O Insignia conseguiu reviver o LIVE do Xbox
original porque a criptografia de 2002 caiu; a de 2005 não caiu.

O servidor tem amigos, presença e chat na API, mas eles vivem no celular/PC, não na
dash.

### Contas na dash original — já existe, e é local

O perfil do 360 é criado inteiramente no sistema original (Configurações → Perfil) e
funciona 100% offline. Só não tem como amarrá-lo a uma conta no seu servidor, pelo
mesmo motivo acima. As contas do EPTX servem para decidir quem pode mandar o quê para
qual console.

---

## Como funciona o envio

O DashLaunch injeta plugins em todo título carregado, **incluindo o dashboard**. Com
um servidor FTP rodando como plugin, o console fica acessível pela rede enquanto a
NXE original está na tela. O servidor então:

1. Conecta no FTP do console
2. Cria `Hdd1:\Content\0000000000000000\<TitleID>\<ContentType>\`
3. Grava o cabeçalho do GOD e cada parte `Data0000`, `Data0001`, ...
4. Você reinicia o console

O reboot do passo 4 não é opcional: a dash monta a lista de conteúdo na inicialização
e não fica olhando o disco. Sem reiniciar, o jogo está no HD mas não aparece.

Arquivo que já está lá com o mesmo tamanho é pulado. Isso transforma um reenvio de
GOD de 8 GB numa operação de segundos quando só faltava uma parte.

## Estado atual

| Parte | Estado |
|---|---|
| Scanner de biblioteca (GOD, XBLA, pasta XEX, ISO) | funcionando |
| Envio por FTP para o console | funcionando, testado contra um FTP falso |
| Contas, catálogo, amigos, presença, chat (API) | funcionando |
| CLI de envio | funcionando |
| Leitor de STFS | pronto, validado por hash contra pacotes reais |
| Gerador de STFS (blocos, tabelas de hash, tabela de arquivos) | pronto, round-trip exato |
| Gerador de STFS (cabeçalho) | pronto |
| Empacotar pasta → container instalável | pronto, com autoverificação |
| Tabela de hash nível 2 (pacotes > 118 MB) | falta |
| Painel web | não começou |
| Conversor ISO → GOD | não começou |

Nada disso foi testado contra um Xbox 360 de verdade ainda — o teste automatizado usa
um servidor FTP de mentira. O primeiro envio real é o que vai dizer se os FTPs do 360
concordam com as suposições do cliente.

---

## Rodando

Precisa de Node 22.5+ (usa o SQLite embutido, sem dependência nativa).

```bash
npm install
```

Copie `.env.example` para `.env` e aponte `EPTX_LIBRARY` para a pasta dos seus jogos.
Gere um segredo próprio:

```bash
node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"
```

### Preparando o console

1. DashLaunch instalado, com o servidor FTP ativo (é um plugin — ele roda por baixo
   da dash original, sem você precisar carregar XeXMenu nem nada).
2. Anote o IP do console em Configurações → Sistema → Configurações de Rede.
3. Usuário e senha padrão do FTP costumam ser `xbox` / `xbox`.

### Enviando um jogo

```bash
npm run scan
```

```bash
npm run send -- --list
```

```bash
npm run send -- --add-console "Sala" 192.168.0.15
```

```bash
npm run send -- --title 3 --console 1
```

O `--add-console` testa a conexão antes de salvar, então se ele reclamar o problema
está no FTP do console, não no servidor.

### Outros comandos

`npm start` sobe a API. `npm run dev` sobe com reload. `npm run smoke` roda o teste
ponta a ponta. `npm run typecheck` roda o TypeScript.

Para inspecionar e validar um pacote STFS:

```bash
npm run stfs -- "D:/Xbox360/MeuJogo/pacote"
```

Alem de listar o conteudo, ele confere o SHA-1 de uma amostra de blocos contra as
tabelas de hash do proprio pacote — o que valida a formula de offset de bloco de
forma criptografica, incluindo as fronteiras de 0xAA onde uma formula errada quebra
primeiro.

Para validar o **gerador** de STFS:

```bash
npm run stfs:roundtrip -- "D:/Xbox360/MeuJogo/pacote"
```

Ele le um pacote real, remonta a regiao de dados do zero e compara byte a byte.
E a unica validacao honesta possivel sem um console: se as tabelas de hash que a
gente produz saem identicas as que a Microsoft produziu, entao posicao,
encadeamento, bytes de status e ordem de hashing estao todos certos — nao ha como
acertar isso por acaso.

Para empacotar uma pasta num container instalavel:

```bash
npm run stfs:pack -- ./minhapasta ./saida --title-id 45450001 --name "Meu App"
```

Depois de gerar ele le o proprio resultado de volta e compara cada arquivo com a
origem, byte a byte.

Quatro detalhes do formato que so apareceram nessas comparacoes, e que documentacao
nenhuma teria entregue:

- Diretorios tambem carregam o bit 0x40 ("contiguo") — sao 0xC3, nao 0x83.
- A tabela de nivel 1 nao fica antes do primeiro grupo: ela vem depois dos dados
  dele, colada na tabela de nivel 0 do grupo seguinte.
- A tabela de topo guarda o total de blocos alocados em 0xFF0, logo apos as
  0xAA entradas. Nas tabelas de nivel 0 intermediarias esse campo fica zerado.
- O SHA-1 do cabecalho, em 0x32C, cobre de 0x344 ate o **fim do bloco** (0xB000),
  nao ate headerSize (0xAD0E). O padding entra no hash.

### Limites conhecidos

- Pacotes acima de ~118 MB precisam de tabela de hash nivel 2, ainda nao
  implementada. O gerador recusa explicitamente em vez de produzir lixo.
- Pacotes abaixo de ~700 KB (um unico grupo de blocos) usam um layout **deduzido**:
  a tabela de nivel 0 vira a de topo. Nao havia nenhum pacote pequeno a mao para
  conferir isso contra um original. Se um pacote pequeno nao instalar, olhe aqui
  primeiro.
- A area de assinatura fica zerada. Num RGH o freeboot nao verifica; num console de
  fabrica isto nao instalaria.

## Formatos reconhecidos pelo scanner

| Formato | Como é detectado | Destino no console |
|---|---|---|
| GOD (Games on Demand) | arquivo STFS + pasta irmã `<nome>.data` | `Hdd1:\Content\0000000000000000\<TitleID>\<ContentType>\` |
| XBLA / DLC / title update | arquivo STFS avulso (magic `CON `/`LIVE`/`PIRS`) | idem |
| Pasta homebrew | diretório contendo `default.xex` | `Hdd1:\Games\<pasta>\` |
| ISO | extensão `.iso` | listado, mas **não instalável** |

Sobre ISO: FATX não aceita arquivo maior que 4 GB, então um ISO de jogo não pode ser
copiado para o HD do console — precisa virar GOD antes. O scanner lista o ISO com
`installable: false` só para você saber que ele está lá, e o envio recusa com uma
explicação em vez de falhar no meio da transferência.

A pasta de instalação não sai de uma tabela chumbada: ela é o campo `contentType` do
próprio cabeçalho, em hexadecimal, que é exatamente o que o console faz. Um tipo de
conteúdo desconhecido ainda vai parar no lugar certo.

---

## API

A API é para o celular e o PC. O console não fala com ela — ele só fica com a dash
aberta e o FTP escutando.

### Autenticação

Não usamos TLS. O motivo é o console: a cadeia de certificados dele é a da Microsoft
e não dá para instalar uma raiz própria sem patchear o `xam.xex`. Como o protocolo
foi desenhado para eventualmente ter um cliente rodando lá, ele assina cada
requisição em vez de depender do transporte.

No login o servidor devolve `token` e `secret`. O `secret` trafega **uma única vez**.
Depois disso toda requisição leva:

| Cabeçalho | Conteúdo |
|---|---|
| `X-EPTX-Session` | o token opaco |
| `X-EPTX-Timestamp` | unix seconds do cliente |
| `X-EPTX-Nonce` | aleatório, único por requisição |
| `X-EPTX-Signature` | HMAC-SHA256 em hex da string canônica |

A string canônica, com `\n` de separador:

```
MÉTODO
/caminho?com=query
TIMESTAMP
NONCE
sha256hex(corpo)
```

O corpo entra como os **bytes exatos que foram enviados**. Reserializar o JSON dá
outro hash e a assinatura falha.

Nonce é de uso único (bloqueia replay) e o timestamp tem janela de 15 minutos por
padrão — larga de propósito, porque o relógio do 360 anda errado com frequência.
`GET /api/v1/time` devolve o horário do servidor para o cliente se alinhar.

### Endpoints

| Método | Rota | Assinado | O que faz |
|---|---|---|---|
| GET | `/health` | não | status do servidor |
| GET | `/api/v1/time` | não | horário do servidor |
| POST | `/api/v1/auth/register` | não | cria conta (o primeiro vira admin) |
| POST | `/api/v1/auth/login` | não | login direto |
| POST | `/api/v1/auth/device/start` | não | inicia pareamento por código |
| POST | `/api/v1/auth/device/approve` | não | aprova pelo celular |
| POST | `/api/v1/auth/device/poll` | não | pergunta se aprovaram |
| GET | `/api/v1/me` | sim | perfil e presença |
| GET | `/api/v1/catalog` | sim | lista títulos (`q`, `kind`, `limit`, `offset`) |
| GET | `/api/v1/catalog/:id` | sim | detalhe com a lista de arquivos |
| GET | `/api/v1/catalog/:id/art` | sim | capa PNG extraída do cabeçalho STFS |
| POST | `/api/v1/catalog/scan` | sim | revarredura (admin) |
| GET | `/api/v1/targets` | sim | consoles cadastrados |
| POST | `/api/v1/targets` | sim | cadastra um console (admin) |
| POST | `/api/v1/targets/:id/test` | sim | testa o FTP sem transferir nada |
| DELETE | `/api/v1/targets/:id` | sim | remove (admin) |
| POST | `/api/v1/push` | sim | envia um título para um console |
| GET | `/api/v1/push/:id` | sim | progresso do envio |
| GET | `/api/v1/push` | sim | envios recentes |
| GET | `/api/v1/download/:fileId` | sim | serve o arquivo com suporte a `Range` |
| POST | `/api/v1/downloads/progress` | sim | registra progresso de download |
| GET | `/api/v1/friends` | sim | amigos com presença |
| GET/POST | `/api/v1/friends/requests` | sim | pedidos de amizade |
| POST | `/api/v1/friends/requests/:id/accept` | sim | aceita |
| DELETE | `/api/v1/friends/:id` | sim | recusa ou remove |
| POST | `/api/v1/presence` | sim | heartbeat |
| GET | `/api/v1/chat/conversations` | sim | conversas com não-lidas |
| POST | `/api/v1/chat/dm` | sim | abre conversa privada |
| GET | `/api/v1/chat/:id/messages` | sim | mensagens, com long-poll |
| POST | `/api/v1/chat/:id/messages` | sim | envia mensagem |
| PUT | `/api/v1/me/avatar` | sim | salva uma imagem (`image/png` até 512x512 e 256 KB, ou `image/gif` até 1024x1024 e 8 MB) |
| POST | `/api/v1/me/avatar/look` | sim | o editor da dash: `{look: [16 números], png: base64}` |
| DELETE | `/api/v1/me/avatar` | sim | volta ao avatar padrão |
| GET | `/avatar/:gamertag/avatar-body.png` | não | o PNG do avatar (o padrão, se a conta não mexeu); 404 se a conta usa GIF |
| GET | `/avatar/:gamertag/avatar-body.gif` | não | o GIF do 360sona; 404 se a conta não tem |
| GET | `/avatar` | não | página para montar no 360sona e mandar o GIF |

### Avatar

O avatar do 360 é desenhado em 3D pela biblioteca de avatares do XDK, que o xsdk
não tem. Cada conta EPTX tem um destes três:

- **Boneco do EPTX** (`look`): um personagem próprio, desenhado pela dash a partir
  de 16 escolhas (pele, cabelo, olhos, óculos, roupa, calçado...). Toda conta nova
  começa com o padrão. Edite em *Meu EPTX → perfil → Editor de Avatar*: cima e
  baixo trocam a parte, esquerda e direita a opção, X sorteia, Y volta ao padrão e
  A salva. A dash manda as escolhas e o corpo desenhado em PNG.
- **GIF do 360sona**: abra `http://<servidor>:8360/avatar` no navegador. A página
  mostra o [360sona](https://360sona.com/) (site de terceiros, aberto ali dentro)
  ao lado de um envio: monte o avatar lá, use *Save as GIF*, entre com a conta EPTX
  e mande o arquivo (até 1024x1024 e 8 MB). Na dash o GIF aparece num quadrinho
  parado no primeiro quadro e roda inteiro enquanto o bloco do perfil (ou do amigo)
  está selecionado. Cada quadro é reduzido para 192 px, e ficam no máximo 48.
- **PNG parado**, mandado pelo comando abaixo.

Os endereços imitam o do Xbox LIVE, mas são do servidor EPTX e respondem pelas
contas EPTX — nada fala com a Microsoft. Quem nunca mexeu no avatar recebe o PNG
do boneco padrão (`src/assets/avatar-default.png`, gravado pelo preview da dash):

```
http://<servidor>:8360/avatar/<gamertag>/avatar-body.png
http://<servidor>:8360/avatar/<gamertag>/avatar-body.gif
```

Para pôr (PNG ou GIF) ou voltar ao padrão pela linha de comando:

```bash
npm run avatar -- --user Rodrigo --pass senha --file meu-avatar.png
```

```bash
npm run avatar -- --user Rodrigo --pass senha --remove
```

`/api/v1/me` e `/api/v1/friends` trazem `avatarType` (`look`, `gif` ou `png`),
`avatarLook` (as escolhas, ou `null`) e `avatarUpdated`; a dash só baixa a imagem
de novo quando ele muda.

`POST /api/v1/push` responde `202` na hora com um `jobId`: um GOD leva dezenas de
minutos no Wi-Fi do 360, e segurar a resposta HTTP até o fim só daria timeout.

### Cliente FTP

Escrito à mão em vez de usar biblioteca pronta. Os FTPs do 360 são implementações
enxutas — sem MLSD, sem TLS, suporte a REST variando de build para build — e um
cliente próprio deixa explícito de quais comandos a gente depende: `USER`, `PASS`,
`TYPE I`, `PASV`, `MKD`, `SIZE`, `STOR`, `QUIT`.

Duas decisões que valem saber:

- O IP anunciado na resposta do `PASV` é ignorado; usamos sempre o host do canal de
  controle. Alguns FTPs do 360 anunciam um endereço interno errado ali.
- `550` no `MKD` é tratado como "já existe", que é o caso comum ao mandar o segundo
  arquivo de um GOD.

---

## Cliente do console

O nucleo do cliente esta em `client/core/`, escrito em C portavel: crypto, JSON,
HTTP e o motor de download. Ele nao sabe nada sobre Xbox — grava em caminhos que o
chamador fornece. Isso permite compilar e testar **o mesmo codigo** no PC contra o
servidor de verdade, o que ja foi feito: o pacote do Minecraft baixou por ele com
SHA-1 identico ao original, e a retomada de download foi verificada truncando o
arquivo no meio.

```bash
npm run client:build
```

```bash
client/build/eptx --host 127.0.0.1 --user Rodrigo --pass senha --list
```

```bash
client/build/eptx --host 127.0.0.1 --user Rodrigo --pass senha --get 2 --out ./baixados
```

Precisa do Zig (`winget install zig.zig`), usado so como compilador C.

### Verificacao de big-endian

O Xbox 360 e PowerPC big-endian. Codigo de crypto escrito displicentemente funciona
no PC e falha no console, e o sintoma e um 401 opaco. Duas defesas:

```bash
npm run selftest:core
```

Confere SHA-256, HMAC e a assinatura completa contra vetores gerados pelo servidor
(`npm run vectors`), e depois compila o nucleo para PowerPC64 big-endian.

Nenhuma leitura de palavra e feita direto da memoria: todo load e store de 32 bits
em `eptx_sha256.c` e byte a byte. E o que faz o digest sair igual nas duas
arquiteturas. O `eptx_selftest()` tambem fica embutido no app, para a primeira
execucao no console confirmar isso de verdade.

### O que falta

| Peca | Estado |
|---|---|
| Crypto e assinatura | pronto, verificado contra o servidor |
| Parser JSON | pronto |
| Cliente HTTP (WinSock/XNet) | pronto |
| Download com retomada | pronto, testado com queda simulada |
| CLI de host | pronto |
| Dash 2.0 estilo NXE (xsdk + Dear ImGui) | compila; visual conferido no preview do PC; falta o console |
| Empacotamento como XBLA | pronto, com autoverificacao |
| Abrir jogo da biblioteca pela dash | experimental, nunca testado |

### A dash do console (xsdk)

`client/xbox/` e o EPTX Live no console: uma dash homebrew no jeito da NXE de
2008, feita com o [xsdk](https://github.com/xenosworks/xsdk) (SDK homebrew de
engenharia reversa publica, sem XDK nem Visual Studio 2010) e desenhada com o
Dear ImGui do [imgui-xenon](https://github.com/xenosworks/imgui-xenon).

| Canal | Blocos |
|---|---|
| Meu EPTX | Perfil (entrar/sair), Biblioteca de Jogos, Downloads, Dashboard Original |
| Loja de Jogos | um bloco por jogo do servidor, com a capa; A abre o detalhe e instala |
| Amigos | amigos do EPTX, online primeiro, com o jogo de cada um |
| Sistema | Servidor, Atualizar, Sobre (licencas) |

Cima e baixo trocam de canal; esquerda e direita escolhem o bloco; A seleciona,
B volta, Y atualiza. **Back abre o Guide do EPTX** (loja, amigos, biblioteca,
downloads, dashboard original, sair). O botao do Xbox continua abrindo o Guide
do sistema: o XAM fica com esse botao e o titulo nem o recebe. Toda a arte e
desenhada no codigo (formas, o avatar e a fonte Selawik, OFL): nada vem da dash
da Microsoft.

- **Perfil:** o bloco do perfil abre o teclado do console para gamertag e senha
  da conta EPTX. A sessao (token e segredo, nunca a senha) fica em
  `Hdd1:\EPTX\session.ini` e a dash entra sozinha na proxima vez. O teclado do
  xsdk nao tem modo senha: ela aparece enquanto voce digita.
- **Presenca:** online, a dash avisa o servidor a cada 60 s, e os amigos veem
  "Online - EPTX Live".
- **Instalacao:** baixa direto em `Hdd1:\Content\...` (`hdd:` para o xsdk). O
  cabecalho do GOD e gravado **por ultimo**: e ele que faz a dash listar o jogo,
  entao um download que cai no meio nao deixa um jogo sem dados. Instalar de novo
  retoma de onde parou.
- **Biblioteca:** le os cabecalhos STFS em `Content\0000000000000000\*\00007000`
  e `000D0000` (nome e icone). "Jogar" chama o loader do XAM com o caminho do
  container; isso nunca rodou num console, e jogo GOD normalmente precisa ser
  montado antes. Se nao abrir, jogue pela dash original.

Para compilar, uma vez, dentro do WSL (Debian ou Ubuntu), siga os passos 1 a 5 do
`docs/GETTING_STARTED.md` do xsdk: clone em `~/xsdk`, toolchain em `~/xsdk-gcc`
(`tools/toolchain/build.sh`) e `make`. Depois, do PowerShell (o `build.sh` clona
o imgui-xenon em `~/imgui-xenon` se faltar):

```bash
wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX Live/client/xbox/build.sh"
```

Para ver a tela sem console, o preview roda o mesmo `dash.cpp` no PC com dados
de exemplo e grava PNGs em `client/build/xbox/preview/`:

```bash
wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX Live/client/xbox/preview/build.sh"
```

### Instalando no console

Abrindo por "Meus Jogos" (recomendado para o primeiro teste):

```bash
npm run client:xbla
```

Isso gera em `client/build/xbox/` um container XBLA (`0xD0000`, `default.xex` e
`licenses/` na raiz) com o content ID como nome. Envie por FTP para
`Hdd1:\Content\0000000000000000\45505458\000D0000\`, preencha
`client/xbox/eptx.ini.example` como `Hdd1:\EPTX\eptx.ini` e reinicie.

Como dash de boot pelo DashLaunch: copie `client/build/xbox/eptx.xex` solto para
`Hdd1:\EPTX\eptx.xex` e aponte o `Default` do `launch.ini` para ele. Solto, o
`game:` e a pasta `Hdd1:\EPTX`, e o log sai em `Hdd1:\EPTX\eptx.log`. **Teste
primeiro por "Meus Jogos"**: o bloco "Dashboard Original" abre
`\SystemRoot\dash.xex`, e isso nunca rodou num console. Se o DashLaunch desviar
esse lancamento para o `Default`, voce volta para o EPTX em vez da NXE. Tenha o
FTP do DashLaunch ligado para conseguir desfazer.

Dentro do pacote XBLA o log vai para `game:\eptx.log`, que la e so de leitura;
nao conferido o que o xsdk faz nesse caso. Para depurar, use o `.xex` solto.

---

## Configuração anti-ban do console

O risco de ban é zero se o console nunca falar com a Microsoft.

- DashLaunch com `liveblock` e `livestrong` ligados.
- `pingpatch` desligado (é para System Link/XBL, e não usamos nada disso).
- DNS do console **não** apontando para um resolver que resolva `*.xboxlive.com`.
- Nenhum endereço da Microsoft em lugar nenhum.

Um sinkhole de DNS embutido ainda não faz parte deste repositório. Enquanto isso, o
`liveblock`/`livestrong` do DashLaunch cobre o essencial.

---

## Próximos passos


1. **subir o Docker e montar libxenon num container para compilar XEX** de verdade na maquina
2. **Testar contra um console de verdade.** É o que falta para sair do "deve
   funcionar". O `--add-console` já diagnostica metade dos problemas possíveis.
3. **Painel web.** Escolher o jogo pelo celular e ver a barra de progresso é o
   fluxo de uso real; hoje isso é CLI.
4. **Conversor ISO → GOD**, para que ISO deixe de ser um beco sem saída.
5. **Sinkhole de DNS** embutido.


## Estrutura

```
src/
├── index.ts            servidor e registro das rotas
├── lib/
│   ├── config.ts       variáveis de ambiente
│   ├── db.ts           schema SQLite
│   ├── crypto.ts       scrypt, HMAC, string canônica
│   ├── session.ts      validação das requisições assinadas
│   ├── stfs.ts         parser do cabeçalho dos containers do 360
│   ├── scanner.ts      varredura da biblioteca
│   ├── ftp.ts          cliente FTP para os servidores do 360
│   ├── png.ts          tamanho e assinatura de PNG (avatar)
│   └── push.ts         orquestra o envio de um título
├── routes/
│   ├── auth.ts         contas, login, pareamento
│   ├── catalog.ts      catálogo e capas
│   ├── download.ts     entrega com Range
│   ├── push.ts         consoles cadastrados e envios
│   ├── social.ts       amigos e presença
│   ├── avatar.ts       avatar da conta, em /avatar/<gamertag>/avatar-body.png
│   └── chat.ts         mensagens com long-poll
└── tools/
    ├── send.ts         CLI de envio
    ├── avatar.ts       CLI para pôr ou tirar o avatar da conta
    ├── scan.ts         varredura manual
    ├── client.ts       cliente de referência da API
    ├── fakeFtp.ts      servidor FTP de mentira, para teste
    └── smoke.ts        teste ponta a ponta
```
