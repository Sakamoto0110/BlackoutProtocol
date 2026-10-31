# Blackout Protocol

[English (en-US)](#en-us) | [Português (pt-BR)](#pt-br)

<a id="en-us"></a>

## English (en-US)

Turn off all monitors with a keyboard shortcut, and **keep** them off.

```
Ctrl + Shift + Alt + F11        toggle blackout
```

### The problem it solves

Putting a monitor to sleep is the easy half. Keeping it asleep is the hard
half: Windows, a driver, a background application, or an accidental mouse
movement can wake the screen a second later.

The program watches for exactly that. It registers for
`GUID_CONSOLE_DISPLAY_STATE` notifications and, while blackout is active, any
notification that the display is returning to `ON` or `DIMMED` triggers another
request to turn it off. Whatever woke the screen loses the contest.

Press the shortcut again to wake the monitors for real.

Only the monitors are affected. The computer stays awake, nothing is locked,
and everything that was running keeps running.

### How waking up works

Waking up is more delicate than it seems. The return path handles three things:

1. The blackout flag is cleared **before** anything wakes the display.
   Otherwise, our own display-on notification would be interpreted as an
   unwanted wake attempt, immediately turning the screens off again.
2. Pending turn-off requests are discarded. A display event may have queued
   a `WM_BLACKOUT_FORCE_OFF` before the shortcut was processed.
3. A one-shot timer confirms the wake request again after 150 ms. Some GPU,
   HDMI, and DisplayPort combinations switch between on and off while the
   link comes up. Without this confirmation, the screen may stay off.

### System tray icon

The program has no visible window. Its only interface is a notification-area
icon with a single menu entry: **Exit**.

Exiting through it goes through `WM_CLOSE`, so the window is destroyed normally
and cleanup runs: the shortcut is unregistered, the power-notification handle
is released, and the tray icon is removed instead of lingering as a ghost.

The icon is recreated if Explorer restarts, through the registered
`TaskbarCreated` message.

### Building

Open `BlackoutProtocol.slnx` in Visual Studio, or run:

```
msbuild BlackoutProtocol.vcxproj /p:Configuration=Release /p:Platform=x64
```

All four configurations build without warnings. They all use the Windows
subsystem: the entry point is `wWinMain`, so the console subsystem will not link.

Requires Windows 8 or later (`_WIN32_WINNT 0x0602`) and a C++20 toolset.

### Notes

- **Single instance.** Launching it twice is a no-op: a named mutex blocks the
  second instance, which exits on its own.
- **Idle when idle.** No polling, worker thread, or repeating timer: the program
  blocks in `GetMessage` until Windows has something to report. The only timer
  confirms the wake request and is stopped as soon as it fires.
- **No elevation required.** Runs as a regular user, without UAC.
- If another application has already registered the shortcut, the program
  reports it at startup and exits instead of running without a working shortcut.

### About RGB lighting

There was an attempt to turn off the motherboard's RGB lighting along with the
monitors using the MSI Mystic Light SDK. It was removed: the public SDK
(`1.0.0.08`, from 2020) cannot write to the Mystic Light 3.x installations bundled
with the current MSI Center. Reads work, but every setter returns `-2`.

The code is preserved in the history in case MSI releases a compatible SDK:

```
git show 27966df:MysticLight.cpp
git show 27966df:MysticLight.h
```

**If you want RGB lighting to turn off with the screens**, most MSI motherboards
already do this automatically when the monitor enters standby. Find the option
in the BIOS or MSI Center and enable it: the effect is the same, with no extra
code involved.

---

<a id="pt-br"></a>

## Português (pt-BR)

Apaga todos os monitores com um atalho, e **mantém** eles apagados.

```
Ctrl + Shift + Alt + F11        alterna o blackout
```

### O problema que ele resolve

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

### Como o retorno funciona

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

### Ícone na bandeja

O programa não tem janela. O único pedaço de interface é um ícone na área de
notificação, com uma entrada só: **Exit**.

Sair por ele passa por `WM_CLOSE`, então a janela é destruída da forma normal
e a limpeza roda: o atalho é desregistrado, o handle de notificação de energia
é liberado, e o ícone sai da bandeja em vez de ficar como fantasma.

O ícone é recriado se o Explorer reiniciar, via a mensagem registrada
`TaskbarCreated`.

### Compilando

Abra `BlackoutProtocol.slnx` no Visual Studio, ou:

```
msbuild BlackoutProtocol.vcxproj /p:Configuration=Release /p:Platform=x64
```

As quatro configurações compilam sem avisos. Todas usam o subsistema Windows —
o ponto de entrada é `wWinMain`, então o subsistema de console não linka.

Requer Windows 8 ou superior (`_WIN32_WINNT 0x0602`) e um toolset C++20.

### Notas

- **Uma instância só.** Abrir duas vezes é no-op; a segunda sai sozinha,
  barrada por um mutex nomeado.
- **Ocioso quando ocioso.** Sem polling, sem thread de trabalho, sem timer
  repetitivo — o programa bloqueia em `GetMessage` até o Windows ter algo a
  dizer. O único timer existe para a confirmação de acordar, e é morto assim
  que dispara.
- **Não precisa de elevação.** Roda como usuário comum, sem UAC.
- Se outro aplicativo já tiver o atalho registrado, o programa avisa no
  arranque e sai, em vez de subir surdo.

### Sobre a iluminação RGB

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
