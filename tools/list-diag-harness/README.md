# Harness do livro-razão, do lote e da publicação atrasada (T3)

Modelo, **fora do emulador**, do protocolo de `docs/list-diag-design.md` (3.3, 3.3.1 e 3.4). Só compila o
`core/gpu_dirty_memory_manager.h` **real** de `src/` (por um stub de `core/device_memory_manager.h`, que dá só o
tamanho da página); o resto é modelo. Nada do emulador roda aqui, e nada foi alterado em `src/`.

## Como rodar

```sh
tools/list-diag-harness/run.sh            # 3000 avisos por núcleo, 3 rodadas (-O2); um quinto nos sanitizers
tools/list-diag-harness/run.sh 1000 2
LIST_DIAG_OUT=/algum/dir tools/list-diag-harness/run.sh
```

Três builds com `-Wall -Wextra -Werror`: `-O2`, `-fsanitize=thread` e `-fsanitize=address,undefined` (LeakSanitizer
ativo, `detect_leaks=1`; confirmado com um programa que vaza de propósito). Sai com código diferente de zero se uma
verificação falhar ou um sanitizer reclamar. O binário aceita `--notices N --rounds R`.

## Arquivos

| Arquivo | Conteúdo |
| --- | --- |
| `gates.h` | Pontos de parada nomeados: uma thread que chega a um portão armado espera até o teste liberá-lo. É assim que cada intercalação é **escolhida**, não esperada |
| `manager.h` | `Manager<bool Diag>`: o `Collect` e o `Gather` do `GPUDirtyMemoryManager` (`src/core/gpu_dirty_memory_manager.h:26-82`). `Diag=false` tem as ordens de memória de hoje; `Diag=true` usa `seq_cst` em `current` e nos contadores `lot_started`, `lot_done`, `gather_active`, `apply_started_lot` e `violation_floor` |
| `ledger.h` | Slots de 64 bytes (`state` com geração e fase, `marks` com geração, `popped`, `applied`, `checked`, `late` e as larguras; payload todo `std::atomic`), `Register` (escritor, no máximo 8 slots, sela o `marks` velho por `exchange`), `Read` (conferência de `state`), `Mark` (CAS esperando a geração, no máximo 2 tentativas), `Free` |
| `system.h` | `CollectObserved` (lote por `(done, started)`, publicação e verificação de publicação tardia), `GatherAll` (os lotes e a junção de faixas com a união dos núcleos), `ApplyBatchRanges` (grava `apply_started_lot` e varre), `Pop` |
| `main.cpp` | Os testes e o estresse |

## O que o modelo faz e o que não faz

- **`cache_mutex`** faz o papel do `buffer_cache.mutex`: o pop e cada aplicação o tomam, e o código do livro-razão não
  acrescenta outra aresta entre eles.
- **P0** (um `Gather` por vez por gerenciador): o estresse a respeita (as threads "frente" e "trás" se revezam sob um
  mutex, que modela o `Drain`); um teste a **viola de propósito** com `skip_gather_on_overlap`, que impede que a
  corrida preexistente do `front_buffer` seja de fato executada.
- **Cerca de leitura:** o leitor usa `atomic_thread_fence(acquire)`. O TSan não a modela (e o GCC a recusa com
  `-Werror=tsan`); sob TSan (`__SANITIZE_THREAD__`) o mesmo ponto usa um RMW `acq_rel` de +0. O `-O2` e o ASan rodam a cerca.
- **Não há** `BufferCache`, `memcpy` do guest concorrente (as amostras usam um guest de mentira), nem tempo real
  (o relógio é um contador que o teste move). O `Hooks` do emulador é aqui um ponteiro de `Gates`.
- **O estresse usa blocos únicos por aviso**, para a verdade de referência (o lote em que o `Gather` entregou o
  bloco) ser inequívoca. A repetição de bloco entre lotes é coberta pelos roteiros determinísticos.

## Os testes

| Teste | O que prova |
| --- | --- |
| `equivalence` | `Manager<false>` e `Manager<true>` entregam o mesmo que o `GPUDirtyMemoryManager` **real**, em 40 000 passos aleatórios |
| `claims` | o lote de cada aviso, nos três caminhos do `Collect` (CAS, `guard`, "já coberto"), com o **mesmo bloco de 64 bytes em vários lotes**. A referência é a identidade escrita no roteiro, **não** "o endereço aparece no callback". Um **mutante** (declara o lote seguinte) falha pela identidade em 5 de 5; a conferência só por endereço é enganada 3 vezes |
| `ambiguous` | retirada em andamento (antes do `guard`), `Gather` entre o `Collect` e a segunda leitura, `Gather` inteiro entre as primeiras leituras e o `Collect`: ambíguo, nunca adivinhado. Com `lot_done` já gravado e a iteração parada, o aviso é do lote seguinte e confere |
| `lot counterexample` | o contraexemplo da revisão: o `Gather` retira A, a CPU registra B no mesmo bloco, a aplicação de A **não** aposenta B (`apply_overlap_other_lot`), e um pop sobre B conta como janela |
| `between the Gather and the application` | o manager está vazio e o livro-razão não: o pop conta (a razão de não haver "peek" no manager) |
| `late publication` | (a) pausa entre `s2` e a publicação, com a aplicação inteira no meio; (b) pausa entre a publicação e a leitura de `apply_started_lot`; (c) aplicação de L pausada entre a gravação de `apply_started_lot` e a varredura; (d) o caso normal. **E o mesmo roteiro sem a verificação** (`naive_no_late_check`), em que a **janela falsa aparece**: prova que o teste é sensível |
| `recycling` | o slot é reaproveitado (i) durante a leitura do payload, (ii) durante a CAS de `t_pop`, (iii) durante a de `t_apply`; (iv-a) payload de outra geração nunca vale como o da atual; (iv-b) escrita atrasada do payload do pop, perdida e depois sobrescrita |
| `overlap`, `apply out of order` | violações de P0: contadas, `violation_floor` sobe, e o pop sobre lote suspeito conta `pop_over_ambiguous` |
| `merged ranges` | a junção de faixas de núcleos diferentes vira um **conjunto** `(núcleo, lote)`; a aplicação aposenta os dois e não o de outro lote |
| `sample labels` | `sample_equal` é consistente com sobrescrita **e** com "o `memcpy` ainda não aconteceu" (limitação 3.5 do desenho) |
| `stress` | escritores por núcleo, um invalidador por vez, uma thread de pop; **verdade de referência** do `Gather`: todo lote inequívoco é o primeiro lote que entregou o bloco; nenhuma janela falsa (um pop nunca marca registro cujo bloco já foi aplicado); conservação: registrados = aplicados + atrasados + expirados + restantes; mais um **mutante** que o estresse tem de ver |

## Achados do harness (já refletidos no desenho)

- **Conservação de fim de vida:** o primeiro desenho contava `notice_expired` na reciclagem só pelo estado
  `publicado`, e um registro já `applied` e ainda não liberado era contado duas vezes (a conservação falhou por 8). A
  reciclagem agora **sela** o `marks` velho por `exchange` e só conta `expired` se o selo não traz `applied` nem `late`
  (`docs/list-diag-design.md`, 3.4, passo 3; linha 9 da seção 11).

## Resultado do último `run.sh`

`run.sh` completo (3000 avisos por núcleo e 3 rodadas no `-O2`; 600 e 2 rodadas no TSan e no ASan/UBSan): `ALL EXPECTATIONS MET` nos três builds (11 606 verificações no `-O2`), sem aviso de sanitizer, LeakSanitizer ativo, saída 0 ("all three builds met every expectation without sanitizer reports").
