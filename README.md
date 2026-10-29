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
notificação de display voltando a `ON` ou `DIMMED` vira um pedido de
desligamento novo. Quem acordou a tela perde a disputa. Aperte o atalho de
novo para acordar de verdade.

Só os monitores são afetados. A máquina continua acordada, nada é bloqueado, e
o que estava rodando continua rodando.

## Ícone na bandeja

O programa não tem janela. O único pedaço de interface é um ícone na área de
notificação, com uma entrada só: **Exit**.

Ele existe por um motivo concreto. Sair pelo menu passa por `WM_CLOSE`, que
executa o encerramento limpo — libera a SDK de iluminação e restaura o que
tiver sido alterado. Matar o processo pula tudo isso.

O ícone sobrevive a um restart do Explorer, via a mensagem registrada
`TaskbarCreated`.

## Iluminação da placa-mãe

> **Aviso: esta parte não funciona em instalações atuais do MSI Center.**
>
> O código está escrito, compila e degrada em silêncio, mas a interface que
> ele usa foi descontinuada. Leia a seção *Limitação conhecida* antes de
> contar com isso.

A ideia era levar o RGB da placa junto com os monitores: apagar as zonas no
blackout, deixar a zona `PCH` piscando em vermelho a meio brilho como
indicador de que o programa está ativo, e devolver tudo ao estado original ao
acordar.

O estado de cada zona — estilo, cor, brilho e velocidade — é gravado em disco
**antes** de qualquer coisa ser apagada:

```
%LOCALAPPDATA%\BlackoutProtocol\ledstate.dat
```

A existência desse arquivo *é* o registro de que as luzes estão apagadas por
causa do programa. Isso torna o caso feio sobrevivível: se o processo morrer
durante um blackout, a execução seguinte encontra o arquivo órfão no arranque
e restaura a partir dele.

Duas decisões de projeto que vale conhecer:

- Acionar o blackout com ele já ativo **não** recaptura o estado. Caso
  contrário o próprio "apagado" seria gravado como estado a restaurar, e o
  original se perderia para sempre.
- Se o arquivo não puder ser gravado, o apagamento é abortado em vez de
  aplicado. Luz que não dá para restaurar é pior que luz acesa.

Há também um log, no mesmo diretório:

```
%LOCALAPPDATA%\BlackoutProtocol\blackout.log
```

Num programa sem janela e rodando elevado, ele é a única forma de descobrir o
que aconteceu depois que aconteceu.

### Limitação conhecida

A SDK pública da Mystic Light (pacote `1.0.0.08`, de 2020) **não escreve** nas
instalações do Mystic Light 3.x que acompanham o MSI Center atual.

O comportamento observado é assimétrico e enganoso:

| Chamada | Resultado |
| --- | --- |
| `MLAPI_Initialize` | `S_OK`, em ~15 ms |
| `MLAPI_GetDeviceInfo` | `S_OK`, instantâneo |
| `MLAPI_GetLedStyle` / `GetLedColor` / `GetLedBright` | `S_OK`, instantâneo |
| `MLAPI_SetLedStyle` / `SetLedColor` / `SetLedBright` | **`-2`, sempre** |

O código `-2` vem rotulado como *"MLAPI_Initialize is timeout"*, o que não tem
relação com a chamada que falhou — é um código genérico mal nomeado.

Registrar `MLAPI_MysticLightControlNotify` antes de escrever, como faz o
exemplo oficial da MSI, não muda nada. O apartamento COM da thread (STA ou
MTA) também não.

O descompasso de versão é a explicação:

| Componente | Versão |
| --- | --- |
| SDK pública usada | 1.0.0.8 |
| `API_Mystic Light.dll` instalada | 3.0.25.5 |
| Pacote MSI Center SDK | 3.2026.x |

O MSI Center 3.x expõe o próprio engine por servidores locais
(`MSI.CentralServer`, `MSI.TerminalServer`, o pipe `MSI_CC_Engine`), mas é
protocolo proprietário e não documentado, que quebraria a cada atualização.

**Na prática:** se a sua placa já apaga o RGB sozinha quando o monitor entra
em standby — comportamento comum em placas MSI, configurável no BIOS ou no
MSI Center — você já tem o efeito desejado sem código nenhum. Deixe essa
opção ligada.

## Requisitos

| | |
| --- | --- |
| Sistema | Windows 8 ou superior (`_WIN32_WINNT 0x0602`) |
| Build | Visual Studio com toolset C++20 |
| Elevação | Exigida pelo manifesto (`requireAdministrator`) |

A elevação existe só por causa da iluminação: sem ela o `MLAPI_GetDeviceInfo`
falha por timeout. Como o manifesto é decidido em tempo de build, o UAC
aparece em todo start, mesmo numa máquina sem a SDK. Autostart silencioso
precisa de uma tarefa no Agendador com *executar com privilégios mais altos* —
a pasta Startup não dá conta.

Se você não usa a parte de iluminação, remover `UACExecutionLevel` do
`.vcxproj` elimina o UAC e não afeta mais nada.

## Compilando

Abra `BlackoutProtocol.slnx` no Visual Studio, ou:

```
msbuild BlackoutProtocol.vcxproj /p:Configuration=Release /p:Platform=x64
```

As quatro configurações compilam sem avisos. Todas usam o subsistema Windows —
o ponto de entrada é `wWinMain`, então o subsistema de console não linka.

A SDK é carregada em tempo de execução por `GetProcAddress`, então nada
precisa ser linkado contra ela e o build não exige que ela esteja presente.
Quando ela é procurada, é sempre por caminho absoluto: o processo roda
elevado, e um `LoadLibrary` por nome simples consultaria o diretório de
trabalho e o `PATH`, o que seria um vetor de sequestro de DLL.

## Notas

- **Uma instância só.** Abrir duas vezes é no-op; a segunda sai sozinha,
  barrada por um mutex nomeado.
- **Ocioso quando ocioso.** Sem polling e sem timer repetitivo. A thread da
  janela bloqueia em `GetMessage` até o Windows ter algo a dizer.
- **Uma thread de trabalho, e só para a SDK.** As chamadas MLAPI conversam com
  o serviço da Mystic Light e podem bloquear por minutos quando esse serviço
  está em mau estado. Rodá-las na thread da janela travaria o laço de
  mensagens, e com ele o caminho `WM_POWERBROADCAST` →
  `WM_BLACKOUT_FORCE_OFF`, que é a razão de existir do programa.
- **Não use `taskkill`.** Matar o processo pula o `MLAPI_Release`, e o serviço
  da Mystic Light fica segurando um cliente que não existe mais. A partir daí
  todo `MLAPI_Initialize` — deste programa ou de qualquer outro — trava, até
  o `Mystic_Light_Service` ser reiniciado.
- Se outro aplicativo já tiver o atalho registrado, o programa avisa no
  arranque e sai, em vez de subir surdo.
