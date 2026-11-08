# MLT + EPTX

Três projetos juntos:

| Pasta | O que é |
| --- | --- |
| [`mlt/`](mlt) | **Bot Strike**: o jogo 5v5 contra bots (`proj/`, Godot) e o porte para o Xbox 360 (`x360/`, C sobre o xsdk) |
| [`eptx-live/`](eptx-live) | **EPTX Live**: o servidor (contas, catálogo, envio por FTP, amigos, chat, salas e relay UDP) e o cliente do console |
| [`eptx-nxe/`](eptx-nxe) | **EPTX NXE Edition**: a dash do console no estilo NXE/FSD |

Só o código-fonte. Ficam de fora: builds (`.xex`, `.exe`, `.pak`, `.zip`), banco de dados, sessões, `.env`, tokens e
qualquer arquivo extraído do Counter-Strike 2 — o Bot Strike lê esses arquivos da cópia do CS2 de cada jogador.
