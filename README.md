# Blackout Protocol

Apaga todos os monitores com um atalho, e **mantém** eles apagados.

```
Ctrl + Shift + Alt + F11        alterna o blackout
```

## O problema que ele resolve

Mandar o monitor dormir é a metade fácil. A metade difícil é ele continuar
dormindo: o Windows, um driver, um aplicativo em segundo plano ou um esbarrão
no mouse acordam a tela um segundo depois.

O programa vigia exatamente isso. Ele se registra em
`GUID_CONSOLE_DISPLAY_STATE` e, enquanto o blackout estiver ativo, qualquer
notificação de display voltando a `ON` ou `DIMMED` vira um novo pedido de
desligamento. Quem acordou a tela perde a disputa.

Aperte o atalho de novo para acordar de verdade.

Só os monitores são afetados. A máquina continua acordada, nada é bloqueado, e
o que estava rodando continua rodando.

## Como o retorno funciona

Acordar é mais delicado do que parece, e o caminho de volta trata três coisas:

1. A flag de blackout é desligada **antes** de qualquer coisa que acorde o
   display. Se a ordem fosse outra, a nossa própria notificação de
   display-ligado seria lida como uma tentativa indevida de acordar, e as
   telas apagariam de novo na hora.
2. Pedidos de desligamento que já estavam na fila são descartados. Um evento
   de display pode ter enfileirado um `WM_BLACKOUT_FORCE_OFF` antes de o
   atalho ser processado.
3. Um timer de disparo único reconfirma o acordar 150 ms depois. Algumas
   combinações de GPU, HDMI e DisplayPort oscilam entre ligado e desligado
   enquanto o link sobe, e sem essa confirmação a tela pode ficar apagada.

## Ícone na bandeja

O programa não tem janela. O único pedaço de interface é um ícone na área de
notificação, com uma entrada só: **Exit**.

Sair por ele passa por `WM_CLOSE`, então a janela é destruída da forma normal
e a limpeza roda: o atalho é desregistrado, o handle de notificação de energia
é liberado, e o ícone sai da bandeja em vez de ficar como fantasma.

O ícone é recriado se o Explorer reiniciar, via a mensagem registrada
`TaskbarCreated`.

## Compilando

Abra `BlackoutProtocol.slnx` no Visual Studio, ou:

```
msbuild BlackoutProtocol.vcxproj /p:Configuration=Release /p:Platform=x64
```

As quatro configurações compilam sem avisos. Todas usam o subsistema Windows —
o ponto de entrada é `wWinMain`, então o subsistema de console não linka.

Requer Windows 8 ou superior (`_WIN32_WINNT 0x0602`) e um toolset C++20.

## Notas

- **Uma instância só.** Abrir duas vezes é no-op; a segunda sai sozinha,
  barrada por um mutex nomeado.
- **Ocioso quando ocioso.** Sem polling, sem thread de trabalho, sem timer
  repetitivo — o programa bloqueia em `GetMessage` até o Windows ter algo a
  dizer. O único timer existe para a confirmação de acordar, e é morto assim
  que dispara.
- **Não precisa de elevação.** Roda como usuário comum, sem UAC.
- Se outro aplicativo já tiver o atalho registrado, o programa avisa no
  arranque e sai, em vez de subir surdo.

## Sobre a iluminação RGB

Houve uma tentativa de levar o RGB da placa-mãe junto com os monitores, via a
SDK da MSI Mystic Light. Ela foi removida: a SDK pública (`1.0.0.08`, de 2020)
não escreve nas instalações do Mystic Light 3.x que acompanham o MSI Center
atual. As leituras funcionam, mas todo setter retorna `-2`.

O código está preservado no histórico, caso a MSI publique uma SDK compatível:

```
git show 27966df:MysticLight.cpp
git show 27966df:MysticLight.h
```

**Se o que você quer é o RGB apagar junto com as telas**, a maioria das placas
MSI já faz isso sozinha quando o monitor entra em standby. Procure a opção no
BIOS ou no MSI Center e deixe-a ligada — o efeito é o mesmo, sem código
nenhum no meio.
