# LSC Codex RTSP

RTSP sidecar para cameras LSC/Tuya baseadas em Anyka, sem substituir o `anyka_ipc` oficial.

Em vez de trocar o binario principal da camera por um build de outro firmware, este projeto sobe um segundo processo em `8554` que le os ring buffers que o firmware stock ja gera em `/tmp` e os reempacota como RTSP.

## Por que este hack e superior

O metodo antigo mais comum para essas cameras troca o `anyka_ipc` inteiro por outro binario com RTSP habilitado. Isso ate pode abrir a `554`, mas costuma misturar branches diferentes de firmware/Tuya e pode fazer o app sumir, perder cloud ou quebrar comportamento de PTZ.

Este projeto segue outra ideia:

- mantem o `anyka_ipc` oficial rodando
- nao tenta abrir o sensor direto com SDK Anyka
- nao toma posse dos recursos de captura do firmware
- so le os buffers ja produzidos pelo processo stock
- publica um RTSP separado em `8554`

Na pratica, o app Tuya continua no caminho oficial e o nosso processo so funciona como um "sidecar".

## Estado atual

O que ja esta provado neste projeto:

- build ARM EABI5 funcionando com a toolchain Anyka original
- binario custom rodando na camera de verdade
- `anyka_ipc` oficial e servidor custom coexistindo ao mesmo tempo
- RTSP em `rtsp://CAMERA_IP:8554/main_ch`
- `ffprobe` reconhecendo:
  `hevc`
  `2304x1296`
  `15 fps`

O que ainda nao esta pronto:

- audio AAC
- `sub_ch`
- multiplos clientes
- autenticacao RTSP
- limpeza completa do empacotamento RTP/HEVC

Limitacao conhecida atual:

- alguns clientes, inclusive `ffprobe`, ainda podem mostrar:
  `Illegal temporal ID in RTP/HEVC packet`

Mesmo assim, no estado atual o stream ja foi identificado corretamente como HEVC ao vivo na camera real.

## Versao testada

Testado em:

- camera LSC rotatable / Tuya baseada em Anyka
- firmware:
  `V3.2863.105`
- modo:
  firmware stock restaurado
  `anyka_ipc` oficial preservado
  servidor custom rodando em `8554`

Durante os testes bem-sucedidos:

- o `anyka_ipc` oficial continuou ouvindo em `6668`
- o nosso processo abriu `8554`
- o caminho oficial do app Tuya permaneceu preservado porque o `anyka_ipc` stock continuou ativo

Observacao importante:

Este projeto parte do pressuposto de que voce ja conseguiu acesso por SD/telnet a camera sem deixar um `anyka_ipc` estrangeiro substituindo o original.

## Como funciona

O fluxo e este:

1. O `anyka_ipc` stock gera os buffers:
   `/tmp/VideoMainStream0`
   `/tmp/VideoSubStream0`
   `/tmp/AudioStream`
2. O servidor deste projeto le o ring buffer principal.
3. Ele interpreta a tabela de frames, os offsets circulares e o wrapper Anyka por frame.
4. Os payloads `type = 129` sao convertidos em pacotes RTP/HEVC.
5. O servidor expoe o stream em:
   `rtsp://CAMERA_IP:8554/main_ch`

Ou seja:

- Tuya continua usando o caminho oficial
- nos so aproveitamos os buffers internos do firmware

## Fontes e creditos

Este projeto nao nasceu do nada. Ele foi desenvolvido com base em reverse engineering proprio e tambem em referencias publicas importantes:

- [guino/LSCOutdoor1080P](https://github.com/guino/LSCOutdoor1080P)
  Base para entender o bootstrap por SD, o ecossistema LSC antigo e os hacks classicos de RTSP.
- [tasarren/lsc-tuya-toolkit](https://github.com/tasarren/lsc-tuya-toolkit)
  Importante para boot hijack, factory mode, comportamento Anyka/Tuya e estrutura geral da inicializacao.
- [Nemobi/ak3918ev300v18](https://github.com/Nemobi/ak3918ev300v18)
  Fonte primaria do SDK Anyka usada para entender como `factory_cfg.ini` e `CONFIG_RTSP_SUPPORT` entram no `anyka_ipc`.
- [Nemobi/Anyka](https://github.com/Nemobi/Anyka)
  Referencia util sobre SDK, demos e comportamento esperado de builds Anyka.
- [MuhammedKalkan/Anyka-Camera-Firmware](https://github.com/MuhammedKalkan/Anyka-Camera-Firmware)
  Usado para comparar apps diretos de captura, dependencias de runtime e mostrar por que abrir o sensor direto nao era o melhor caminho para coexistencia com Tuya.
- [ricardojlrufino/arm-anykav200-crosstool](https://github.com/ricardojlrufino/arm-anykav200-crosstool)
  Fonte da toolchain cruzada usada para gerar o binario ARM compativel.
- [seydx/tuya-ipc-terminal](https://github.com/seydx/tuya-ipc-terminal)
  Nao e usado na camera, mas serviu como referencia de fallback host-side para Tuya -> RTSP.

O desenho final deste repositorio, porem, e diferente dessas referencias:

- nao substitui `anyka_ipc`
- nao usa o SDK para capturar direto do sensor
- usa leitura dos ring buffers do firmware stock

## Pre-requisitos

Para compilar:

- Ubuntu 22.04 ou WSL similar
- `bash`
- `gcc`
- `python3`
- `wget`
- `rsync`
- `dpkg-deb`
- `bubblewrap` (`bwrap`)
- `ffprobe` ou `ffplay` para validacao

Para instalar na camera:

- acesso telnet funcional a camera
- acesso de escrita a SD montada em `/tmp/sd`
- firmware stock com `anyka_ipc` original ativo

## Estrutura do repositorio

- `src/anyka_ring_rtsp_server.c`
  servidor RTSP sidecar
- `scripts/build_host.sh`
  build x86_64 para validar offline
- `scripts/build_anyka.sh`
  build ARM EABI5 para a camera
- `scripts/setup_i386_runtime.sh`
  baixa runtime i386 local para destravar a toolchain antiga
- `scripts/setup_i386_root.sh`
  monta a arvore minima de libs i386
- `scripts/sync_anyka_build_env.sh`
  sincroniza toolchain + runtime para `~/lsc-build-env`
- `scripts/run_anyka_toolchain_bwrap.sh`
  executa a toolchain Anyka dentro de `bwrap`
- `scripts/make_deploy_reader8554_telnet.sh`
  gera o script de deploy via telnet
- `scripts/deploy_live.sh`
  faz build, gera deploy e envia para a camera
- `scripts/start_reader8554_live.sh`
  snippet camera-side para iniciar o servidor ao vivo
- `scripts/stop_reader8554.sh`
  snippet camera-side para parar o servidor
- `scripts/show_reader8554_log.sh`
  mostra o log do processo na camera
- `tools/telnet_exec.py`
  helper para enviar comandos/scripts via telnet

## Preparando a toolchain

Este projeto usa a toolchain antiga da Anyka, que tem binarios host i386.

Em vez de instalar pacotes i386 globalmente no sistema, os scripts deste repositorio criam um runtime local e executam a toolchain dentro de `bwrap`.

### 1. Baixe ou extraia a toolchain

Voce precisa de um diretorio extraido que contenha algo como:

```text
.../usr/bin/arm-anykav200-linux-uclibcgnueabi-gcc.br_real
```

Voce pode usar como base o repositorio:

- [ricardojlrufino/arm-anykav200-crosstool](https://github.com/ricardojlrufino/arm-anykav200-crosstool)

Importante:

- preserve symlinks
- extraia no WSL/Linux, nao pelo Explorer do Windows

### 2. Aponte `ANYKA_TOOLCHAIN_SRC`

Exemplo:

```bash
export ANYKA_TOOLCHAIN_SRC="$HOME/src/arm-anykav200-crosstool/arm-anykav200-crosstool"
```

O valor precisa ser o diretorio que contem `usr/bin`, `usr/libexec` e o sysroot da toolchain.

### 3. Faca o build ARM

```bash
bash scripts/build_anyka.sh
```

O binario gerado ficara em:

```text
out/anyka_ring_rtsp_server_arm
```

## Build host

Para validar offline em x86_64:

```bash
bash scripts/build_host.sh
```

Isso gera:

```text
out/anyka_ring_rtsp_server_host
```

## Validacao offline no host

Se voce tiver um dump do ring buffer principal:

```bash
./out/anyka_ring_rtsp_server_host \
  --ring /caminho/para/VideoMainStream0.full \
  --port 8554 \
  --static-replay \
  --verbose
```

Em outro terminal:

```bash
ffprobe -rtsp_transport tcp -show_streams rtsp://127.0.0.1:8554/main_ch
```

## Instalacao na camera

### Metodo simples

Se voce ja tem telnet funcionando:

```bash
bash scripts/deploy_live.sh 192.168.1.126
```

Esse script:

- faz o build ARM
- gera `out/deploy_reader8554_to_sd.telnet`
- envia o binario para `/tmp/sd/anyka_ring_rtsp_server`
- sobe o processo em:
  `8554`

### Metodo manual

1. Gere o script de deploy:

```bash
bash scripts/make_deploy_reader8554_telnet.sh
```

2. Envie para a camera:

```bash
python3 tools/telnet_exec.py 192.168.1.126 --wait 15 --file out/deploy_reader8554_to_sd.telnet
```

3. Verifique:

```bash
ffprobe -rtsp_transport tcp -show_streams rtsp://192.168.1.126:8554/main_ch
```

## Uso

URL principal:

```text
rtsp://CAMERA_IP:8554/main_ch
```

Exemplo com `ffplay`:

```bash
ffplay -rtsp_transport tcp rtsp://192.168.1.126:8554/main_ch
```

Exemplo com `ffprobe`:

```bash
ffprobe -rtsp_transport tcp -show_streams rtsp://192.168.1.126:8554/main_ch
```

## Operacao manual via telnet

Para iniciar manualmente na camera:

```bash
python3 tools/telnet_exec.py 192.168.1.126 --file scripts/start_reader8554_live.sh
```

Para parar:

```bash
python3 tools/telnet_exec.py 192.168.1.126 --file scripts/stop_reader8554.sh
```

Para ver o log:

```bash
python3 tools/telnet_exec.py 192.168.1.126 --file scripts/show_reader8554_log.sh
```

## Evidencia do teste ao vivo

Durante o teste real que validou o projeto, `ffprobe` retornou:

```text
codec_name=hevc
width=2304
height=1296
avg_frame_rate=15/1
```

E na camera os processos coexistiam assim:

```text
8554 -> anyka_ring_rtsp_server
6668 -> anyka_ipc
```

## Limitacoes atuais

- video apenas
- apenas `main_ch`
- um cliente por vez
- sem autenticacao
- empacotamento RTP/HEVC ainda precisa refinamento
- ainda existe o aviso:
  `Illegal temporal ID in RTP/HEVC packet`

## Seguranca e risco

Este projeto e bem menos invasivo que substituir o `anyka_ipc`, mas ainda e um hack.

Recomendacoes:

- nao mexa em flash se nao for necessario
- mantenha um caminho de recuperacao por reboot
- use a SD como area de staging
- teste primeiro com telnet, nao automatize no boot antes de validar

## Recuperacao

Se algo der errado:

- mate o processo:
  `killall anyka_ring_rtsp_server`
- ou reinicie a camera

Como o `anyka_ipc` original nao e substituido por este projeto, a recuperacao e muito mais simples do que nos hacks de troca de binario principal.

## Proximos passos

- corrigir o detalhe de RTP/HEVC que gera o aviso de temporal ID
- adicionar AAC usando os frames `type = 130`
- expor `sub_ch`
- melhorar multiplos clientes
- opcionalmente integrar start automatico por SD sem tocar no binario oficial
