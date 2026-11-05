# Harness do diagnóstico `EDEN_LIST_DIAG` (T3a)

Compila e roda, **fora do emulador**, o **mesmo código** que o emulador usa: `src/tests/video_core/list_diag.cpp` (o
arquivo Catch2 do CMake do emulador) contra os headers reais `src/video_core/list_diag/*.h`, o
`src/core/gpu_dirty_memory_manager.h` real (a variante `Diag=true` de verdade) e o `Common::RangeSet` real. Não há
modelo nem cópia do protocolo. Ver `docs/list-diag-design.md` (8.1.1).

## O que vem de `src/` e o que é stand-in

| Vem de `src/` (real) | Stand-in (só aqui) |
| --- | --- |
| `video_core/list_diag/protocol.h`, `ledger.h`, `counters.h`, `hooks.h` | `stub/core/device_memory_manager.h`: só `DEVICE_PAGEBITS` (o `gpu_dirty_memory_manager.h` real não precisa de mais nada dele) |
| `video_core/list_diag.h` (a chave, o estado, os envoltórios dos ganchos) | `sink_stub.cpp`: o destino do log (`InstallLogSink`); no emulador é `video_core/list_diag.cpp` (`LOG_INFO`); aqui as linhas vão para o stderr |
| `core/gpu_dirty_memory_manager.h` (`CollectT`/`GatherT`, `GatherLotState`) | |
| `common/range_sets.h` e `.inc` (Boost) | |
| `tests/video_core/list_diag_legacy_manager.h`: o `Collect`/`Gather` de **antes** da mudança, só para o teste de equivalência | |

## Como rodar

Precisa de Catch2 3 (`libcatch2-dev`) e Boost (`libboost-dev`).

```sh
tools/list-diag-harness/run.sh          # 3000 avisos por núcleo no estresse (-O2); um quinto nos sanitizers
tools/list-diag-harness/run.sh 1000
LIST_DIAG_OUT=/algum/dir tools/list-diag-harness/run.sh
```

Três builds com `-Wall -Wextra -Werror`: `-O2`, `-fsanitize=thread` e `-fsanitize=address,undefined` (LeakSanitizer ativo,
`detect_leaks=1`; confirmado com um programa que vaza de propósito). Sai com código diferente de zero se um teste falhar ou
um sanitizer reclamar. `LIST_DIAG_STRESS_NOTICES` ajusta o estresse.

## Cerca de leitura sob TSan

O leitor do livro-razão usa `atomic_thread_fence(acquire)`. O TSan não a modela (e o GCC a recusa com
`-Werror=tsan`): sob `__SANITIZE_THREAD__` o mesmo ponto usa um RMW `acq_rel` de +0. O `-O2` e o ASan rodam a cerca.

## Os testes

Ver `docs/list-diag-design.md`, 8.1 e 8.1.1. Cada intercalação é **escolhida** com pontos de parada nomeados (`Gates`:
uma thread que chega a um portão armado espera até o teste liberá-lo), não esperada por sorte. Os testes incluem a
fronteira de geração (65534 a 131072), a violação de P0 com o relatório, `blk` e `pop`, a chave, "desligado, nada muda e
nada é alocado", e um estresse com verdade de referência vinda do `Gather` e um mutante. As asserções só rodam na thread
principal.

## Achados do harness (já refletidos no desenho)

- **Conservação de fim de vida** (revisão 2): reciclar um slot `publicado` já aposentado contava `expired` duas vezes;
  a reciclagem agora sela o `marks` velho por `exchange`.
- **Geração truncada** (revisão 3, A): o `Free` comparava a etiqueta de 16 bits com a geração completa, e a etiqueta 0
  era tratada como "sem payload". Antes da correção o teste de fronteira falhava 6 vezes (geração 65536 e 65537); depois,
  0; a mutação que reintroduz o bug é pega na geração 65536.
- **Custo da varredura** (revisão 3): o índice de slots vivos (bitmap) existe porque a varredura por faixa lia os
  6 × 1024 slots.

## Resultado do último `run.sh`

`run.sh` completo (estresse com 3000 avisos por núcleo no `-O2` e 600 nos sanitizers): 23 casos e 318 asserções passam nos três builds (`-O2`, TSan, ASan+UBSan), sem aviso de sanitizer, LeakSanitizer ativo, saída 0 ("all three builds passed every test without sanitizer reports").
