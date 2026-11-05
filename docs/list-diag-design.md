# T3: build de diagnóstico (`EDEN_LIST_DIAG`), desenho proposto

**Estado: T3a (partes `pop` e `blk`) implementado no emulador, desligado por padrão, medido (8.4: −20% de draws/s no FE, cobertura de ~20%); T3a.1 (amostragem por página e caminho quente) em desenho; T3b (`list`) e T3c (produtores estrangeiros) não começaram.** Revisão 3 (seção 11, linhas 10 a 13). Escrito em 2026-11-05 sobre `740b932`;
revisado sobre `0fc35ad` e de novo sobre `2aafcf1` (branch `claude/bold-dirac-tppzm7`), como parte de `docs/handoff-contracts-prototype.md`,
T3. As revisões incorporadas estão na seção 11.

Graus de certeza como nos outros docs: **verificado** (aberto e lido, com `arquivo:linha`), **medido** (de
quem), **hipótese**. Caminhos relativos a `src/`.

## 0. Alcance (vale para tudo o que este documento e o T3 dizem)

- **No harness:** as duas perdas de dados foram reproduzidas (`held_write_vs_pending_download` e
  `held_write_block_clobbers_gpu_bytes`) e as proteções modeladas passaram (`..._protected` e
  `held_write_block_exact_notices`).
- **No emulador:** há caminhos plausíveis identificados por leitura, **sem ocorrência nem frequência medidas**.
- **A correção de produção está em aberto.** O positivo do harness **não decide** qual mecanismo integrar.
- **Princípio (usuário):** uma intercalação encontrada e reproduzida justifica a correção; a medição no jogo
  serve para **dimensionar exposição e custo**, e uma execução sem ocorrências **não a invalida**
  (`docs/gpu-produced-data-plan.md`, 12.2).
- **Linguagem:** o hook de registro fica **depois do `Collect` e antes do `memcpy`** (`core/memory.cpp:587-590` e
  `:649-653`, **verificado**). O que o diagnóstico vê é um **aviso registrado**, nunca uma **escrita concluída**.
  Todo nome, contador e log abaixo diz "aviso registrado → pop → aplicação". Essa limitação afeta a
  **interpretação do T3a inteiro** (seção 3.5), não só a terceira intercalação.

O build de diagnóstico **só observa**. Ele não corrige nada, e não pode eliminar a janela que mede.

## 1. Chave de ativação e partes

`EDEN_LIST_DIAG` (padrão: desligado; lida uma vez na partida, como `EDEN_BACK_THREAD_STATS`,
`video_core/back_thread_stats.cpp:15-21`). Valores:

| Valor | Partes ligadas |
| --- | --- |
| ausente, vazio ou `0` | nenhuma |
| `1` | `pop`, `blk` e `list` |
| lista separada por vírgulas | só as nomeadas: `pop` (janela do pop), `blk` (blocos de 64 bytes), `list` (entradas de lista, cópias, escrita perdida) |

**Por que partes separadas:** `pop` e `blk` não criam cópia, barreira nem trabalho na GPU; `list` cria cópias e
barreiras no fluxo do Vulkan, o que **muda o tempo das fences e, com isso, a probabilidade da janela do pop**. A
primeira medição da janela deve usar `EDEN_LIST_DIAG=pop,blk`; `list` é medida à parte (seção 8).

O estado é um `std::atomic<u32>` lido com `relaxed` e **não muda durante a sessão** em produção (só os testes o
mudam, com o sistema parado). Sem a variável: uma leitura e um desvio por ponto de observação, nenhuma cópia,
buffer ou trava nova (seção 7).

## 2. Arquivos e interfaces

| Arquivo | O quê |
| --- | --- |
| `video_core/list_diag/protocol.h`, `ledger.h`, `counters.h`, `hooks.h` (novos, **header-only e autocontidos**) | **Uma implementação só**, incluída pelo emulador e pelos testes, sem cópia: o livro-razão (3.4), `CollectObserved`, `GatherAll`, `MakeBatch`, `BeginApply`/`EndApply`, `NoteApply` (aposentadoria e `blk`), `NotePop`, a fila de eventos de log com limites, a violação de P0 e os relatórios. `Hooks` é uma política de modelo (`ProdHooks` no emulador: sem pontos de parada, relógio de microssegundos) por onde os testes injetam pontos de parada, um relógio e os mutantes (só com `kTest = true`: compilado para fora em produção) |
| `video_core/list_diag.h` / `.cpp` (novos) | a chave (`EDEN_LIST_DIAG`, `g_parts`), o estado **alocado só quando uma parte liga** (`State()`), os envoltórios que os pontos de gancho chamam (`CollectNotice`, `NoteApplyHook`, `NotePopHook`, `DrainLog`, `LogSummary`), cada um com a leitura da flag e um desvio no início; `list_diag.cpp` só instala o destino do log (`LOG_INFO`) |
| `core/gpu_dirty_memory_manager.h` | **só acréscimos:** `Core::GatherLotState` (uma linha de cache por núcleo, **fora do gerenciador**: o gerenciador mantém o layout de hoje, conferido por `static_assert` e por teste), `DirtyNoHooks`, e `CollectT<Diag, Hooks>` e `GatherT<Diag, Hooks>`. `Collect` e `Gather` viraram chamadas finas de `CollectT<false>` e `GatherT<false>`: a instanciação `Diag=false` tem as ordens de memória e o código de hoje; a `Diag=true` usa `seq_cst` em `current` e nos contadores (3.3). O estado do lote entra por parâmetro, não por membro |
| `core/memory.cpp:765` | **1 ponto:** `ListDiag::CollectNotice(gpu_dirty_managers[core], core, address, size)`; desligado, chama o `Collect` de sempre depois da leitura da flag |
| `video_core/gpu.cpp:123-163` (`InvalidateGPUCache`) | ramo do diagnóstico, que **não altera** o vetor `cached_writes` que a junção produz: `GatherAll` com `(núcleo, lote)` (usa `System::GetGPUDirtyMemoryManager()`, que já existia: `core.cpp` e `core.h` **não mudam**), `MakeBatch` depois da junção, `BeginApply`/`EndApply` ao redor de `OnCachedWrites` (o lote publicado numa variável `thread_local`) e o log descarregado depois (1 por segundo, fora de todas as travas dos caches); o resumo final vai em `NotifyShutdown` |
| `video_core/buffer_cache/buffer_cache.h:141` | **1 ponto:** no topo de `CachedWriteMemory` (antes do `return` de região não registrada e antes de qualquer mudança): `ListDiag::NoteApplyHook(device_addr, size, gpu_modified_ranges, cpu_read_ranges, !AsyncBufferDownloads(), leitura do guest)` |
| `video_core/buffer_cache/buffer_cache.h:719-722` | **1 ponto:** no laço do pop, antes do `WriteBlockUnsafe`: `ListDiag::NotePopHook(start, end, bytes)` |
| `video_core/back_thread_stats.cpp` | o bloco "[ListDiag]" no `Report()` (totais e variação por segundo, janela de 4 s), vazio com o diagnóstico desligado. Os contadores moram no estado do diagnóstico, não em `Shared` |
| `video_core/CMakeLists.txt`, `tests/CMakeLists.txt` | os arquivos novos |
| `src/tests/video_core/list_diag.cpp` (novo), `list_diag_legacy_manager.h` | **um só arquivo Catch2**, compilado pelo CMake do emulador e por `tools/list-diag-harness/run.sh` (seção 8); o `list_diag_legacy_manager.h` é o `Collect`/`Gather` de antes, só para a equivalência |
| `video_core/dma_pusher.cpp:140-165`, `buffer_cache.h:1735` (só `list`, **T3b, não feito**) | registro por entrada e `Defer`; tabela por página em `MarkWrittenBuffer` |

Os pontos de `pop` e `blk` são: `memory.cpp:765`, `gpu.cpp` (ramo do diagnóstico), o laço do pop e o topo de
`CachedWriteMemory`. Nada muda no que os caches executam.

## 3. Observar sem interferir

### 3.1 O que o diagnóstico não pode fazer

- Nenhuma trava nova que **ordene** o pop em relação à aplicação dos avisos. O pop e a aplicação já são
  serializados pelo `buffer_cache.mutex` (o pop em `fence_manager.h:286-293`, a aplicação em
  `vk_rasterizer.cpp:972-981`, **verificado**); o diagnóstico não acrescenta aresta entre eles.
- Nenhuma aplicação antecipada, nenhum `Gather` extra: o `Gather` **consome** os avisos
  (`core/gpu_dirty_memory_manager.h:56-82`) e mudaria o que mede. As chamadas ao `Gather` que o diagnóstico faz
  são **as mesmas** que o emulador já faz (a mesma chamada, em variante que também anota o lote).
- Nenhuma espera. Nada do diagnóstico bloqueia o caminho de draws, o pop, a aplicação ou a escrita da CPU.

### 3.2 Por que a thread de fences não "espia" o `GPUDirtyMemoryManager`

**Não lê o `GPUDirtyMemoryManager`.** Duas razões (**verificadas** no código):

1. Ele não tem leitura que não consuma: o `Gather` troca os buffers (`:61`) e o `current` (`:60`). Um "peek"
   teria de tomar o `guard` (`:34,59`), que a escrita da CPU também toma no caminho lento do `Collect`.
2. **Um peek erraria por falta.** Depois do `Gather` os avisos já saíram do manager, mas ainda **não foram
   aplicados**: a aplicação roda depois, em `OnCachedWrites` (`gpu.cpp:134-162`, `vk_rasterizer.cpp:958-992`),
   tomando o `buffer_cache.mutex` só ali. É um dos instantes em que o pop pode cair, e o manager já não o mostra.

Por isso há um **livro-razão próprio do diagnóstico** (3.4), com o intervalo **exato** que a CPU escreveu (o
`Collect` o perde ao arredondar para 64 bytes, `:113-121`) e com o **lote** a que cada aviso pertence (3.3).

### 3.3 Vínculo entre aviso e aplicação: o lote

**Precondição P0: um `Gather` por vez por gerenciador, e as aplicações de um gerenciador na ordem das retiradas.**
O `guard` do `Gather` protege só a troca dos buffers (`core/gpu_dirty_memory_manager.h:59-64`); a iteração e o
`clear()` do `front_buffer` ficam **fora** dele (`:66-81`, **verificado**). Dois `Gather` concorrentes no mesmo
gerenciador seriam uma **corrida preexistente** sobre o `front_buffer`, independente do livro-razão. O inventário de
todos os chamadores, com a thread de cada um, com e sem split, está em `docs/gpu-contract-inventory.md`, 7.3. Resumo,
**confirmando a leitura do planejador, com uma ampliação**:

- **Com split**, `gpu_thread.cpp:79` (`Defer`), `:82` (`Defer(TickWork)`, que roda o pedido síncrono do `OnCPURead`,
  `gpu.cpp:264-266`), `fence_manager.h:139` (fim de `SignalFence`, na thread de quem a chama), `puller.cpp:339` e
  `maxwell_3d.cpp:435` rodam na **trás**; **sem split**, na **thread única**; a **thread de fences não chama**
  (`ReleaseThreadFunc`, `fence_manager.h:241-272`).
- **A frente também chama, sempre depois de um `Drain`:** `dma_pusher.cpp:176-183` (a exceção que a revisão apontou) e,
  **além dela**, `puller.cpp:225`, `:252`, `:259`, `:264` e `:291`, que chamam `ReleaseFences` direto
  (`WaitPendingFences`, `SignalFence`, `InvalidateGPUCache`), depois do `DrainBack` de `:186` e de `:285`. Depois do
  `Drain` a trás fica ociosa até a frente enviar mais (`puller.cpp:178-181`), então as chamadas não se sobrepõem.
- Isso é **convenção do `Drain`, não trava**: nada a verifica. Não abri `GpuSplit::MaybeStart` e `Stop`, o chamador
  de `query_cache.h:586` nem o dispatch de `maxwell_3d.cpp:435`.

**A precondição vale nos caminhos lidos**, então é escrita como precondição do protocolo, e o harness a respeita (as
threads "frente" e "trás" se revezam sob um mutex que modela o `Drain`). Se algum caminho a violar, é **possível falha
preexistente**, registrada em `docs/gpu-contract-inventory.md` (7.3) e **separada do livro-razão**: o diagnóstico **não
a corrige nem a esconde**. Ele a vigia com `gather_active` (abaixo), conta `gather_overlapped` e `apply_out_of_order`, e
trata como **ambíguos** os lotes até ali (`violation_floor`).

**P0 violada invalida o diagnóstico da sessão, não só alguns lotes (revisão 3, C).** Na **primeira** ocorrência de
`gather_overlapped` ou de `apply_out_of_order` (`CheckViolation`, chamada depois de cada `Gather` e de cada início de
aplicação, no log e no resumo), o diagnóstico: (1) escreve **uma vez** no log, em destaque, `precondicao P0 violada: os
resultados do diagnostico a partir daqui nao sao confiaveis`; (2) prefixa com `[P0 VIOLATED: results after this are not
reliable]` todas as linhas do relatório periódico seguintes; (3) repete o aviso no **resumo final**; (4) guarda um
**instantâneo de todos os contadores** do instante da violação, que o resumo imprime (`counters at the first
violation`), para o leitor separar o que veio antes do que veio depois. Os contadores continuam. **Não há trava
corretiva** no T3a.

**Por que endereço e sobreposição não bastam (contraexemplo da revisão, confirmado no código).** O `Gather` retira
o lote A de um gerenciador; a CPU registra o aviso B no mesmo bloco de 64 bytes; a aplicação de A encontra B no
livro-razão por sobreposição e o aposentaria, embora B ainda esteja no manager, e seria dado como "aplicado" um
aviso que ninguém aplicou (e o pop seguinte o ignoraria). Endereço e sobreposição não identificam o lote.

**Proposta (a da revisão, com duas mudanças que a demonstração exigiu):** dois contadores por gerenciador (por
núcleo), só do diagnóstico, em `Core::GatherLotState` (`core/gpu_dirty_memory_manager.h`), **uma linha de cache por
núcleo, num vetor paralelo fora do gerenciador e alocado só quando o diagnóstico liga**:

- `lot_started`: incrementado pelo `Gather` **antes** de tomar o `guard`.
- `lot_done`: gravado **depois** da troca do `current` e dos buffers, **ainda sob o `guard`**. Vale o número do
  lote que esse `Gather` retirou (`lot_done + 1` lido sob o `guard`, que é quem serializa as retiradas). O número
  do lote é, portanto, a **ordem das retiradas**, e não a ordem dos incrementos de `lot_started` (dois `Gather`
  concorrentes podem tomar o `guard` na ordem inversa dos incrementos; numerar pelo incremento daria o número
  errado).

- `gather_active`: incrementado na entrada do `Gather` e decrementado **depois** da iteração e do `clear()` do
  `front_buffer`. Entrar com `gather_active != 0` é a violação de P0 (`gather_overlapped`); o `lot_done` só diz que a
  retirada terminou, e a iteração (a parte sem `guard`) vai além dele, então ele sozinho não vê dois `Gather` que se
  sobrepõem na iteração. A violação sobe o `violation_floor` do gerenciador para o `lot_started` do instante.

**Por que o `gather_epoch` atual não serve (confirmado):** é um `fetch_add` `relaxed` no início do `Gather`
(`:57`), sem ordem com o `exchange` de `current` (`:60`): o incremento pode ficar visível depois da troca, e não
diz quando a retirada terminou.

**Ordem de memória escolhida (variante `Diag=true`).** `seq_cst` em: `lot_started` (incremento e leituras),
`lot_done` (gravação e leituras) e em `current` (o `load`, o `compare_exchange` e o `exchange` do `Collect`/`Gather`).
O resto (`guard`, `back_buffer`) não muda. A variante `Diag=false` mantém `acquire`/`release`/`relaxed` de hoje.
O custo da variante `seq_cst` (em x86-64 e em ARM) **não é afirmado aqui**: é **a verificar por inspeção do código
gerado ou por medição** (seção 7, linha b). Ela só roda com o diagnóstico ligado.

**Protocolo do escritor (`CollectObserved`, em torno do `Collect` real):**

1. `d = lot_done.load()`; `s = lot_started.load()`. Se `s != d`, há retirada em andamento: **lote ambíguo**.
2. `Collect` real (variante `Diag=true`, qualquer dos três caminhos).
3. `s2 = lot_started.load()`. Se `s == d` e `s2 == s`: o aviso é do **lote `d + 1` deste núcleo** (inequívoco).
   Senão: **lote ambíguo**.
4. Registra no livro-razão `(núcleo, lote d+1)` ou `(núcleo, ambíguo)`, junto com o intervalo exato e o carimbo.

**Por que (3) é demonstrável.** Use a ordem total `S` das operações `seq_cst`:

- **(P1) Nenhum lote ≤ `d` contém o aviso.** A leitura `seq_cst` de `lot_done` que viu `d` lê a gravação `seq_cst`
  feita sob o `guard` pelo `Gather` que retirou o lote `d`: tudo daquela retirada acontece antes do passo 2. E
  `s == d` na leitura 1 diz que **todos** os `Gather` iniciados (`d` deles) terminaram, já que `lot_done ≤
  lot_started` sempre.
- **(P2) Nenhuma retirada entre o passo 1 e o fim da inserção.** Uma retirada exige um incremento antes. Para cada
  `Gather` `G` que incrementa `lot_started` depois do passo 1 (`W2`) e retira em seguida (`E`, o `exchange` de
  `current`; `W2` vem antes de `E` na ordem do programa): se `W2` precede a leitura 3 em `S`, a leitura 3 o vê
  (`seq_cst` lê a última gravação anterior em `S`) e `s2 != s`: ambíguo. Senão, a leitura 3 precede `W2` em `S`.
- **(P3) O primeiro `Gather` depois da inserção a inclui.** Caminho **CAS** (`current` por `compare_exchange`): a
  inserção precede a leitura 3 (ordem do programa), que precede `W2`, que precede `E`; todos `seq_cst` em `S`, e
  `S` é coerente com a ordem de modificação de `current`: `E` lê um valor igual ou posterior ao da inserção.
  Caminho **`guard`** (`back_buffer.emplace_back` e `current.exchange`, `:33-37`): a seção crítica do `Collect` e
  a do `Gather` se excluem. Se a do `Gather` vem antes da nossa, `W2` acontece-antes da leitura 3 (o `Gather`
  incrementa antes de tomar o `guard`, e soltar o `guard` sincroniza com nosso `lock`), logo a leitura 3 o vê:
  ambíguo. Se vem depois, enxerga o `back_buffer` e o `current` que inserimos. Caminho **"já coberto"** (`:42-44`,
  retorno sem inserir): o aviso fica coberto pelo que a leitura `seq_cst` de `current` viu (`Z`). Pelo mesmo
  argumento de `S`, a primeira retirada depois dessa leitura lê `Z` ou um valor posterior (a máscara só cresce até
  a retirada, ou `Z` passa a `back_buffer`, que a mesma retirada leva), então os bytes dele vão no lote
  `d + 1`.
- **(P4)** Combinando P1 a P3: se `s == d` e `s2 == s`, o primeiro lote retirado depois da inserção é o `d + 1`, e
  contém o aviso; nenhum anterior o contém.

O que **não** é demonstrado vira ambíguo, nunca adivinhado. Perde-se só quando há retirada concorrente (o caso
raro, e contado).

**Como (núcleo, lote) chega até o `NoteApply`, inclusive com a junção.** O callback do `Gather` entrega só
`(endereço, tamanho)` (`gpu.cpp:131-134`) e `System::GatherGPUDirtyMemory` itera os gerenciadores sem identificá-los
(`core/core.cpp:644-646`, **verificado**); nada disso muda no caminho desligado. No ramo do diagnóstico:

1. `System::GatherGPUDirtyMemoryLots` chama a variante `Diag=true` de `Gather` de cada gerenciador (os 6, como hoje,
   `NUM_CPU_CORES = 6`, `core/hardware_properties.h:22`) com um callback que anota o núcleo, e devolve
   `lots[núcleo]` (o lote retirado por cada um; `0` se o gerenciador não retirou nada).
2. O ramo guarda uma cópia da lista **antes** da junção, com a máscara de núcleos de cada faixa. A junção
   (`EDEN_HELD_WRITE_MERGE`, `gpu.cpp:143-161`) roda **sem mudança** no vetor que vai para `OnCachedWrites`; o
   diagnóstico, **depois**, calcula para cada faixa final a união das máscaras das faixas de antes contidas nela.
   Assim a junção de faixas de núcleos diferentes vira um **conjunto** `{(núcleo, lotes[núcleo])}` por faixa final.
3. O `ApplyBatch` (faixas finais, conjuntos) é publicado por um ponteiro `thread_local` válido só durante a chamada
   a `OnCachedWrites`, que roda na mesma thread (`gpu.cpp:162`; é o único chamador, `grep` em `src/`, **verificado**).
   O `NoteApply(addr, size)` do topo de `CachedWriteMemory` procura nele a faixa **de mesmo `(addr, size)`** (com a
   junção desligada pode haver faixas idênticas: o conjunto é a união). Sem `ApplyBatch` (chamada fora do ramo),
   conta `apply_unlinked` e **não aposenta nada**.

**Regra de aposentadoria.** O `NoteApply` aposenta só os registros cujo `(núcleo, lote)` pertence ao conjunto da
faixa **e** cujo intervalo exato está contido nela. Registros que **sobrepõem** a faixa mas são de outro lote são
contados à parte (`apply_overlap_other_lot`) e **não** são aposentados; os de lote ambíguo, idem
(`apply_overlap_ambiguous_lot`); os que só cruzam a borda, `apply_partial`. Nenhum é aposentado por palpite; o
não aposentado expira (3.4).

**Efeito do lote ambíguo nos outros contadores:** um pop sobre registro ambíguo **não afirma janela**: conta
`pop_over_ambiguous` (o registro pode já ter sido aplicado sem que o diagnóstico soubesse). O `blk` só desconta
escritas exatas do **mesmo lote**; com lote ambíguo (ou outro lote) dentro da faixa, o resultado do `blk` é
**"ambíguo"** (seção 5).

**Contagem real das chamadas** (sem presumir uma por lista ou fence): cada `InvalidateGPUCache` chama o `Gather` dos
6 gerenciadores, vazios inclusive (`core/core.cpp:644-646`). `InvalidateGPUCache` roda no início de cada lista
(`gpu_thread.cpp:79,106`), no fim de cada `SignalFence` (`fence_manager.h:139`), no pedido síncrono do `OnCPURead`
(`gpu.cpp:266`, via `TickWork`), em `puller.cpp:339` e em `maxwell_3d.cpp:435`, em mais de uma thread **mas nunca ao
mesmo tempo** (P0; inventário 7.3). O diagnóstico conta: `invalidate_calls`, `invalidate_empty`,
`gather_calls[núcleo]`, `gather_empty[núcleo]`, `gather_overlapped` (um `Gather` que começou com outro em andamento:
`lot_started - lot_done > 0` antes do incremento) e `invalidate_thread_switches` (o chamador difere do anterior).

### 3.3.1 Publicação atrasada: aviso publicado depois da aplicação do seu lote

**Contraexemplo (revisão 2).** O `CollectObserved` lê `s2` (lote L inequívoco) → o escritor é pausado → o `Gather`
retira L → a aplicação de L termina sem achar o registro → o escritor publica o aviso de L como pendente → um pop o
acha: **janela falsa**. A regra de aposentadoria da 3.3 não basta: a aplicação já passou.

**Proposta da revisão, avaliada e adotada, com um acréscimo (`checked`):**

- Por gerenciador, `apply_started_lot` (atômico, `seq_cst`), gravado por quem aplica **antes** de varrer o livro-razão
  (no ramo do diagnóstico de `InvalidateGPUCache`, depois do `Gather` e antes de `OnCachedWrites`): "aplicando até o
  lote L". Só cresce; um lote que não é maior que o gravado é violação de P0 (`apply_out_of_order`, `violation_floor`).
- O escritor **publica** o registro (`state = publicado`, `seq_cst`) e **só depois** lê `apply_started_lot`
  (`seq_cst`). Se `apply_started_lot >= L`, o registro é **"publicado tarde"**: ele mesmo o marca `late` e o libera, e
  **nunca conta como pendente**. Senão, o marca `checked` (pendente confirmado).
- **Demonstração (Dekker, `seq_cst`).** Escritor: `P` (publicar), depois `R` (ler `apply_started_lot`). Aplicador: `W`
  (gravar `apply_started_lot = L`), depois a varredura `V` (a leitura `seq_cst` de `state` de cada slot). Na ordem
  total `S` das operações `seq_cst`: ou `R` precede `W`, e então `P < R < W < V`, e a varredura vê o registro
  publicado (e o aposenta pela regra da 3.3); ou `W` precede `R`, e então `R` lê `L` ou mais: "publicado tarde". Não
  há terceira saída. A varredura lê `state` com `seq_cst` (custo a verificar, como em 7).
- **Acréscimo (`checked`).** Entre `P` e o fim de `R` o registro está publicado e **não confirmado**: um pop que o
  achasse aí afirmaria uma janela que pode ser falsa (o caso "publicado tarde"). Por isso o pop **só afirma janela**
  para registro `checked`; para o publicado e não conferido conta `pop_over_unchecked` e **não afirma**. A aplicação,
  ao contrário, aposenta também o não conferido (é o que a Dekker pede). Cada registro tem **um** fim de vida: `applied`
  (a varredura) ou `late` (o escritor), decidido por CAS em `marks`.
- **Um valor monotônico basta** só por P0: sob a precondição, as aplicações de um gerenciador terminam em ordem, então
  "aplicando até L" implica que todo lote ≤ L já foi (ou está sendo) varrido. Se `gather_overlapped` ou
  `apply_out_of_order` indicar violação, os registros do intervalo (lote ≤ `violation_floor`) são **ambíguos**: o pop
  não afirma e a aplicação não aposenta. Um histórico de aplicações não foi preciso e não foi adotado.
- **Testes determinísticos** (`tools/list-diag-harness`, 8.1.1): pausa entre `s2` e a publicação; pausa entre a
  publicação e a leitura de `apply_started_lot`; aplicação de L concorrente com cada uma (inclusive pausada entre a
  gravação de `apply_started_lot` e a varredura); e o mesmo roteiro **sem** a verificação (`naive_no_late_check`), em
  que a janela falsa **aparece**, para provar que o teste é sensível.

### 3.4 Protocolo de propriedade e geração dos slots

Um "seqlock" sozinho não basta: há **expiração** (um slot publicado e velho é reaproveitado pelo escritor) e os
**leitores marcam** slots (`t_pop`, `t_apply`). O protocolo:

**Geração e etiqueta (correção da revisão 3, A).** A geração de um slot tem 62 bits e é o que `state`, o `Handle` e o
`Free` usam; só `marks` e o payload do pop levam a **etiqueta**, de 16 bits e **nunca zero** (`Tag(g) = g % 65535 + 1`:
zero significa "sem payload" e "nunca publicado"), e só as operações que comparam uma etiqueta a tiram da geração. O
primeiro desenho guardava no `Handle` só os 16 bits baixos e o `Free` os comparava com a geração completa: a partir da
geração 65536 a liberação do registro tardio falhava, o registro ficava `publicado` e a conservação quebrava; além
disso a etiqueta 0 era tratada como "sem payload" e a amostra do pop se perdia a cada 65 536 gerações. A etiqueta
repete a cada **65 535** gerações de um slot (o limite de ABA).

**Índice de slots vivos (achado de custo, revisão 3).** O pop e a aplicação varrem o livro-razão **uma vez por faixa**,
sob o `buffer_cache.mutex`; varrer os 6 × 1024 slots alongaria justamente as seções que o diagnóstico quer medir. Cada
anel tem um bitmap dos slots publicados, e a varredura custa o número de registros vivos. Ordem: o escritor **publica
`state` e DEPOIS liga o bit** (`seq_cst`), então a varredura que vem depois da leitura de `apply_started_lot` pelo
escritor vê o bit (a Dekker da 3.3.1 continua valendo: `P < bit < R < W < V`). Quem libera (`Free`) desliga o bit e
**relê o `state`**: se um escritor reaproveitou o slot nesse meio, o bit volta. O bit é um índice: quem varre ainda lê
o `state` de cada slot.

**Anéis.** Um por núcleo, 1024 slots de 64 bytes (6 × 1024 × 64 B = 384 KiB), alocados só com `pop` ou `blk`. O
escritor de cada anel é único (o `sys_core` é serializado por `sys_core_guard`, `core/memory.cpp:709-717`), o que
não dispensa o CAS: ele protege contra um escritor futuro e contra o reaproveitamento por expiração.

**Slot** (8 palavras de 64 bits, **todas `std::atomic`**; toda leitura e gravação de payload é `relaxed`: não há
corrida de dados no sentido do C++; conferir a sequência depois **não** torna válida uma leitura não atômica
concorrente, por isso o payload inteiro é atômico):

| Palavra | Conteúdo |
| --- | --- |
| `state` | geração (48 bits) e fase (2 bits): `livre`, `escrevendo`, `publicado` |
| `marks` | geração (16 bits baixos), `popped`, `applied`, `checked`, `late`, e as larguras `t_pop` e `t_apply` em unidades de 2 µs desde `t_write`, 22 bits cada (saturam em ~8,4 s; a expiração é 5 s). **Uma palavra só**, com a geração |
| `addr`, `meta` (tamanho, núcleo, ambíguo), `lot`, `t_write` | payload escrito só pelo escritor |
| `pop1`, `pop2` | payload do pop, **cada palavra com a geração (16 bits) junto do dado**: `pop1` = geração, início da amostra e tamanho; `pop2` = geração e hash de 48 bits |

**Escritor** (limitado: examina até 8 slots a partir do cursor):
1. Acha um slot `livre`, ou `publicado` e **expirado** (`agora - t_write > 5 s`; só conta em `notice_expired` se o
   registro velho não tinha terminado: passo 3).
2. `CAS(state: g, fase → g+1, escrevendo)`. Perdeu: tenta o próximo. Sem slot após 8: **descarta** o aviso e conta
   `notice_overflow` (nunca espera).
3. Grava o payload (`relaxed`), **sela** o `marks` velho por `exchange` para `(g+1, 0, ...)` (`seq_cst`) e publica
   `state = (g+1, publicado)` (`seq_cst`). O valor que o `exchange` devolve diz se o registro velho já tinha terminado
   (`applied` ou `late`): **só se não tinha** a reciclagem conta `notice_expired`. Quem marcar depois do selo falha
   pela geração. **O harness achou aqui uma falha do primeiro desenho** (seção 11, linha 9): contar `expired` só pelo
   estado `publicado` contava duas vezes um registro já aposentado e ainda não liberado (a conservação "registrados =
   aplicados + atrasados + expirados + restantes" falhou por 8).

**Leitor** (pop ou aplicação; tentativas limitadas):
1. `st = state.load(seq_cst)` (a varredura da aplicação precisa disso: 3.3.1); só segue se `publicado` com geração `g`.
2. Lê o payload (`relaxed`), depois `state.load` de novo (precedido de `fence(acquire)`; o TSan não modela cercas, e no harness sob TSan a mesma
   ordem vem de um RMW `acq_rel` de +0): se mudou, a leitura é
   **descartada** (`observation_lost`).
3. Marca por **CAS em `marks`** esperando a geração `g` que leu e o bit ainda desligado; **no máximo 2 tentativas**.
   Como `marks` carrega a geração, um slot reciclado (geração nova) faz a CAS falhar: o leitor **nunca marca uma
   geração nova**; conta `observation_lost`. O pop grava `pop1`/`pop2` (marcados com `g`) **antes** da CAS de
   `marks`; se um slot foi reciclado nesse meio, a gravação atrasada deixa `pop1`/`pop2` com geração velha, e o
   leitor da geração nova os trata como ausentes (`unverified`, contado). Isso **nunca produz um valor errado**:
   só uma observação perdida.
4. A aplicação, depois de marcar `applied` e emitir o evento (se o registro foi atingido por pop), tenta
   `CAS(state: (g, publicado) → (g, livre))`. Falhou: nada (o escritor recicla por expiração).

**Perde-se, e se conta:** reciclagem durante a leitura do payload, durante a CAS de `t_pop` e durante a de
`t_apply` (`observation_lost`), anel cheio (`notice_overflow`), expiração (`notice_expired`). **ABA:** a geração de
`marks` tem 16 bits; um slot reciclado 65 536 vezes entre a leitura e a CAS de um leitor passaria como o mesmo
(improvável a ponto de o custo de evitá-lo não valer; declarado).

### 3.5 Aviso registrado não é escrita concluída (limitação do T3a inteiro)

O registro vem **depois** do `Collect` e **antes** do `memcpy` no guest (`HandleRasterizerWrite` roda dentro de
`GetPointerImpl`, que devolve o ponteiro; o `memcpy` é do chamador: `core/memory.cpp:587-590`, `:649-653`).
Consequências para a interpretação de **tudo** o que o T3a mede:

1. **A janela medida é "aviso registrado, ainda não aplicado"**, e não "escrita concluída, ainda não aplicada". Um
   pop que cai entre o registro e o `memcpy` é contado como atingindo um aviso e **não** perde nada (o `memcpy` vem
   depois e vence o pop). Os contadores de pop são, por isso, um **limite superior** da exposição.
2. **A comparação de amostras (4) é confundida pela mesma limitação.** `sample_equal` na aplicação é consistente
   com "a escrita nova foi sobrescrita pelo pop", **e também** com "o `memcpy` ainda não aconteceu quando a aplicação
   leu". `sample_different` diz só que a amostra difere da do pop.
3. **A aplicação pode vir antes do `memcpy`** (terceira intercalação): o upload leva os bytes antigos e **nenhum
   aviso novo corrige isso** (**hipótese**, lida e não executada). Fica **sem hook novo**: medi-la exigiria um hook
   depois do `memcpy` no caminho quente de toda escrita da CPU. O diagnóstico a vê, no máximo, de forma indireta
   (`sample_equal` na aplicação sem pop anterior), e **não distingue**.
4. **Caso no harness (proposto, não implementado nesta revisão):** `held_write_apply_before_memcpy`, o `CpuWriteHeld`
   separado em "aviso" e "bytes" (hoje são os passos i-b e i-c do README, na mesma operação); roteiro: aviso,
   `ApplyHeld`, bytes; negativo: o host termina com os bytes antigos (`WordMismatch`). Sem positivo (não há
   proteção modelada). Serve para dar chão à limitação, e não decide a correção.

### 3.6 O que o diagnóstico pode deixar de ver

| Ponto cego | Efeito | Contador |
| --- | --- | --- |
| Aviso registrado **não** é escrita concluída (3.5) | contagens de pop são limite superior; amostras confundidas; a terceira intercalação não é vista | (limitação; sem contador) |
| O registro vem **depois** do `Collect` real: por alguns ns o aviso existe nos caches e não no livro-razão | um pop nesse intervalo não é contado (subconta) | (não observável; **hipótese**: pequeno) |
| Lote ambíguo (retirada concorrente) | o aviso não é aposentado nem afirmado como janela | `notice_ambiguous`, `pop_over_ambiguous`, `apply_overlap_ambiguous_lot` |
| Violação de P0 (dois `Gather` ou aplicações fora de ordem) | os lotes até ali viram ambíguos; **a corrida em si não é corrigida nem escondida** | `gather_overlapped`, `apply_out_of_order` |
| Publicado e não conferido (3.3.1) | o pop não afirma janela sobre o registro | `pop_over_unchecked` |
| Anel cheio, expiração, reciclagem durante leitura ou marcação | observação perdida | `notice_overflow`, `notice_expired`, `observation_lost` |
| Hash de 48 bits e amostra de até 64 bytes | `sample_equal` pode ser acidental; a cobertura é parcial quando a interseção passa de 64 bytes | `sample_bytes` e `overlap_bytes` (soma) |
| Largura em unidades de 2 µs, saturando em 8,4 s | larguras grosseiras, nunca ordem | (soma e máximo, nunca ordem) |
| Escritas da CPU fora do caminho do aviso segurado (páginas sem dados da GPU: `WriteMemory`, `buffer_cache.h:132-138`; escritas de serviços HLE) | fora do problema medido | não entram |
| Pop de outros caches (textura, queries) | não instrumentados | listado na seção 9 |
| Carimbos TSC entre núcleos | ruído de dezenas de ciclos | só somas e máximos |

**Perturbação que sobra** (dita, não eliminada): na thread da CPU do jogo, três leituras `seq_cst` de contadores, a
inserção no anel e as ordens `seq_cst` de `current` por aviso segurado; no `Gather` (em qualquer thread), um
incremento antes do `guard` e uma gravação dentro dele; no pop e na aplicação, uma varredura dos slots pendentes,
sob o `buffer_cache.mutex` que já está tomado. Isso **alonga levemente** a seção do pop e a da aplicação, e pode
deslocar a probabilidade da janela; não impõe ordem entre elas. A medição de custo (8.3) mede isso.

**Alternativa descartada:** mutex folha no registro. Não ordena o pop contra a aplicação, mas põe uma aresta nova
na thread da CPU do jogo, e **não resolveria o vínculo com o lote** (que exige os contadores do `Gather`).

## 4. Problema A: janela do pop (contadores e logs próprios)

**Definição do evento** (item 1a): *um pop (download antigo publicado) que grava bytes cobertos por um **aviso
registrado ainda não aplicado**.* Registrados: a **faixa do download** (a que o pop grava: `start`, `end` do laço
`buffer_cache.h:719-722`), a **faixa da escrita** (o intervalo exato do livro-razão) e a **ordem** (aviso
registrado, pop, aplicação), com as larguras. "Ainda não aplicado" vale só para registros de **lote
inequívoco**, aposentados pela regra da 3.3.

**Amostra (barata, só quando há sobreposição):** o pop guarda um hash (FNV-1a, 48 bits) de até 64 bytes da
interseção que **ele gravou**. Na aplicação o hook lê do guest os mesmos bytes (`ReadBlockUnsafe`, como
`CachedWriteMemory` faz em `:154`) e compara. **Os três resultados não dizem "perda" nem "benigno":**

- `sample_equal`: a amostra lida na aplicação é igual à do download. **Consistente** com sobrescrita (e com "o
  `memcpy` ainda não aconteceu", 3.5).
- `sample_different`: a amostra na aplicação difere da do download. Só isso.
- `unverified`: não leu (faixa não legível, `pop1`/`pop2` ausentes ou de outra geração, amostra vazia).

Todo resultado sai **com o tamanho da amostra e a cobertura** (`sample_bytes` de `overlap_bytes`).

| Contador (`Shared.list_diag`, relaxed) | O que conta |
| --- | --- |
| `notice_registered`, `notice_applied`, `notice_late`, `notice_expired`, `notice_overflow`, `notice_ambiguous` | avisos registrados e seu destino: cada registrado termina em **um**: `notice_applied`, `notice_late`, `notice_expired`, ou continua pendente (ambíguo ou de outro lote) |
| `pop_ranges` | faixas que o pop gravou no guest (denominador) |
| `pop_over_notice` | faixas de pop que cruzam ao menos um aviso registrado, não aplicado e de lote inequívoco |
| `pop_over_notice_bytes`, `pop_over_notice_records` | bytes da interseção; avisos distintos atingidos |
| `pop_over_ambiguous` | faixas de pop que cruzam só avisos de lote ambíguo (**não afirma janela**) |
| `pop_over_unchecked` | faixas de pop que cruzam avisos publicados e ainda não conferidos (3.3.1; **não afirma janela**) |
| `window_sample_equal`, `window_sample_different`, `window_unverified` | resultado na aplicação (acima); `sample_bytes` e `overlap_bytes` (somas) |
| `notice_to_apply_us` (soma, máx, n) | do registro à aplicação, de **todo** aviso aposentado (dimensiona a exposição mesmo sem ocorrência) |
| `notice_to_pop_us` (soma, máx, n) | do registro ao pop, nos atingidos |
| `apply_overlap_other_lot`, `apply_overlap_ambiguous_lot`, `apply_partial`, `apply_unlinked` | regra de aposentadoria (3.3) |
| `gather_overlapped`, `apply_out_of_order` | violações de P0 (3.3); sobem o `violation_floor` |
| `observation_lost` | reciclagem ou CAS perdida (3.4) |

**Log** (rótulo `[ListDiag][pop]`), uma linha por aviso atingido, emitida na aplicação (com a tríade completa); um
atingido que expira sai com `apply=never`:

```
[ListDiag][pop] notice=[0x..,+N) core=C lot=L download=[0x..,0x..) overlap=N order=notice@t0 -> pop@+Aus -> apply@+Bus sample=sample_equal|sample_different|unverified (sample=Sb of overlap=Ob)
```

## 5. Problema B: blocos de 64 bytes (contadores e logs próprios, separados do A)

**Definição:** *a aplicação de um aviso segurado cujo bloco de 64 bytes cobre bytes ainda em
`gpu_modified_ranges`* (`core/gpu_dirty_memory_manager.h:93-96`; `buffer_cache.h:141-158` e `2105-2128`). O hook
roda no topo de `CachedWriteMemory`, **antes** de `InlineMemoryImplementation` subtrair o trecho de
`gpu_modified_ranges` (`:2107-2108`), e vê o estado que a subtração vai destruir.

Registrados: **endereço e tamanho do trecho aplicado** (o bloco, ou a união de blocos da mesma página depois da
junção de `gpu.cpp:138-161`: é o que o cache enxerga), as **escritas exatas** do **mesmo lote** dentro dele e os
**bytes da GPU cobertos**: `gpu_modified_ranges ∩ trecho` e, descontadas as escritas exatas, os **bytes da GPU
fora do que a CPU escreveu** (`at_risk`). Com `!AsyncBufferDownloads()` (Switch 2), separa os que estão em
`cpu_read_ranges` dos que **não estão** (`not_seen_cpu_read`: "fora de `cpu_read_ranges` neste rastreio"; **não
prova** que os bytes só existem no host: `cpu_read_ranges` não é a história completa do que a CPU leu).

**Resultado ambíguo.** O `blk` só desconta escritas exatas **do mesmo lote**. Se há dentro do trecho um registro de
outro lote, de lote ambíguo, ou nenhum registro (overflow), o resultado do `blk` desse trecho é **"ambíguo"**:
`gpu_covered` continua sendo contado (é limite superior), mas `at_risk` não é afirmado.

"Em risco" é o **limite superior** da perda: só há perda se o valor da GPU difere do que o guest tem naqueles
bytes, e o diagnóstico não lê o buffer do host (isso é a parte `list`, só para entradas de lista).

| Contador | O que conta |
| --- | --- |
| `blk_applies` | trechos aplicados por `CachedWriteMemory` |
| `blk_applies_over_gpu` | os que cruzam `gpu_modified_ranges` |
| `blk_bytes_range`, `blk_bytes_exact` | bytes do trecho; bytes escritos (mesmo lote) |
| `blk_gpu_bytes_covered` | `gpu_modified_ranges ∩ trecho` (limite superior) |
| `blk_gpu_bytes_at_risk` | só nos trechos **não ambíguos**: a parte fora das escritas exatas |
| `blk_at_risk_cpu_read`, `blk_at_risk_not_seen_cpu_read` | em Switch 2: dentro / fora de `cpu_read_ranges` (com `AsyncBufferDownloads()`: `unknown`) |
| `blk_ambiguous` | trechos de resultado ambíguo |

**Log** (`[ListDiag][blk]`): `range=[0x..,+N) blocks=N/64 exact=[(0x..,n)...] gpu_covered=C at_risk=R|ambiguous
(cpu_read=a not_seen_cpu_read=b)`.

**Independência:** `blk` não usa o pop nem os campos `t_pop`; usa o livro-razão (escritas exatas e lote) e o
estado do cache. Pode rodar sem `pop` (a contagem de `Gather`, o lote e o livro-razão rodam do mesmo jeito).

## 6. Entradas de lista (`list`: as letras a a d do item 2 do T3)

Só liga com `list` (ou `1`). **Não muda o que é executado.** A frente nunca consulta caches nem o scheduler
(`docs/architecture-plan.md`, 3.3): ela monta um registro e enfileira um pedido no ring.

1. **Registro da entrada (frente, `DmaPusher::Step`).** Depois de escolher as palavras (snapshot, memória
   atual sem flush, ou com flush: `dma_pusher.cpp:141-165`), copia as palavras usadas para um registro do
   diagnóstico (só `memcpy` e carimbo), com: fonte (`snapshot`, `resumed_unsafe`, `no_snapshot_unsafe`, `safe`),
   deslocamento de retomada, endereço, estado do `dma_state` no início da entrada e a **última espera de host**
   vista pelo puller (tipo, valor, posição), guardada só com a parte ligada. O registro vai por
   `GpuSplit::Defer` (`gpu_split.h`: "Runs `func` on the back after everything sent before it"), ou direto sem o
   split.
2. **Pedido ordenado (trás).** O pedido roda na posição da entrada. Confere em `gpu_modified_ranges` se alguma
   página da entrada tem bytes da GPU; se tem, grava um `CopyBuffer` GPU → **buffer privado do diagnóstico** (padrão
   do `CommitAsyncFlushesHigh`, `buffer_cache.h:672-687`: staging mapeado, `PreCopyBarrier`, `PostCopyBarrier`) só
   das faixas da GPU, e anota `scheduler.CurrentTick()` (`vk_master_semaphore.h:32`). **Não baixa para o guest e
   não mexe em `gpu_modified_ranges`.**
3. **Conclusão por tick, sem esperar.** `IsFree(tick)` (`vk_master_semaphore.h:42`) é consultado depois, sem
   bloquear (a cada `InvalidateGPUCache`); livre, lê o staging, compara com as palavras da frente e registra:
   deslocamento, classe (cabeçalho, dado de método, parâmetro de macro: `ClassifyWords`, segunda passada **fora** do
   laço de `ProcessCommands`), método e subcanal, tipo e posição do último produtor da faixa (tabela abaixo) e a
   última espera de host. A conclusão é a **da própria cópia**, nunca o valor do syncpoint (decisão 4 do handoff).
4. **Escrita perdida.** A tabela "último escritor por página" (atualizada em `MarkWrittenBuffer`, só com a parte
   ligada) guarda `{serial, tipo}`. A frente carimba o `serial` já gravado pela trás quando leu a entrada (uma
   leitura atômica, sem espera). No pedido, a trás confere as páginas: serial maior que o carimbado = produtor
   enviado antes da entrada e gravado **depois** de a frente ler. Conta mesmo com a página sem marca na leitura.
5. **Pontos cegos (item d).** O diagnóstico lista, por observação, os produtores que **não** passam por
   `MarkWrittenBuffer` e que estão em `video_core/`: query/semáforo (`query_cache.h:285-310`), release de QMD
   (`kepler_compute.cpp:169-174`), DMA no caminho lento (`maxwell_dma.cpp:143-149`), `BufferClear` e upload inline
   (`buffer_cache.h:283-304`, `vk_rasterizer.cpp:1220-1269`) e o reset indireto na CPU (`vk_rasterizer.cpp:260-332`).
   São seis pontos de uma chamada (`NoteForeignWrite(tipo, addr, size)`) que alimentam a mesma tabela por página.
   **Não cobre** (dito no relatório): imagens, render targets e aliases (P6), escritas de HLE (P13 a P15, fora de
   `video_core/`), P4 sem origem da GPU e P1 além dos bytes escritos (inventário, 3.1).

| Contador | O que conta |
| --- | --- |
| `list_entries` (por fonte) | entradas vistas |
| `list_candidates` | entradas em página com bytes da GPU |
| `list_copies`, `list_copies_skipped_cap` | cópias gravadas; puladas pelo teto |
| `list_compared`, `list_differing` | cópias lidas; com diferença |
| `list_diff_words` (por classe) | palavras diferentes: cabeçalho, dado de método, parâmetro de macro |
| `list_missed_writes` (com e sem espera de host antes) | produtor gravado depois da leitura da frente, na página da entrada |
| `foreign_writes` (por tipo) | produtores fora de `MarkWrittenBuffer` que cruzaram uma página de entrada |

**Perturbação, dita:** barreiras e cópias a mais no fluxo mudam o tempo (`docs/gpu-produced-data-plan.md`, 4.1.2).
Por isso `list` é medida **à parte** de `pop` e `blk`.

## 7. Declaração da seção 2.2 (T3)

| | |
| --- | --- |
| Condição nova | Observação: `EDEN_LIST_DIAG`. Nenhuma condição de execução nova |
| Esperas novas | **Nenhuma.** Nada do diagnóstico bloqueia pop, aplicação, escrita da CPU, frente ou trás. Escritor e leitor do livro-razão têm tentativas limitadas (8 slots; 2 CAS): contenção vira `observation_lost` ou `notice_overflow` |
| Recursos retidos | `pop`/`blk`: 6 anéis × 1024 slots × 64 B = **384 KiB**, alocados só com a parte ligada; fila de eventos de log (fixa, ~256 eventos). `list`: staging privado e cópias em voo **com teto** (16 MiB e 64 entradas; acima, a entrada é pulada e contada); tabela por página (2^16 × 16 B = 1 MiB) |
| Caminhos afetados | thread da CPU do jogo (por aviso segurado); qualquer thread que chame `InvalidateGPUCache`; thread de fences e quem aplica (varredura sob o `buffer_cache.mutex` já tomado); a frente do split **só** monta o registro e usa o ring |
| Teste | seção 8 |

**Custo sem uso e custo fora do diagnóstico dos contadores do `Gather` (pergunta da revisão):**

| | Desligado | Ligado |
| --- | --- | --- |
| (a) Leitura da flag e desvio | **uma** leitura `relaxed` e um desvio em `System::GatherGPUDirtyMemory`/`InvalidateGPUCache` (por chamada, **não** por gerenciador: o ramo fica fora do laço dos 6) e um em `memory.cpp:765` (só no caminho do aviso segurado), no laço do pop (por faixa), no topo de `CachedWriteMemory` | idem, mais o trabalho abaixo |
| (b) Contadores do lote (`GatherLotState`) | **não são gravados e não existem**: ficam num vetor paralelo, **alocado só quando uma parte liga** (uma linha de cache por núcleo, 6 × 64 B), **fora do `GPUDirtyMemoryManager`**, cujo layout e cujo `Collect`/`Gather` são os de hoje (`CollectT<false>` e `GatherT<false>`; conferido por `static_assert` de `sizeof` e por teste de equivalência). **Não há custo de memória nem de layout fora do diagnóstico.** (A decisão 3 previa os 40 bytes no gerenciador como plano B; não foi preciso.) Gravar sempre continua só com medição que o justifique | seis `std::atomic<u64>` por núcleo, todos `seq_cst` (incremento e leituras; gravação de `lot_done`; `apply_started_lot` é um `store` por gerenciador a cada `InvalidateGPUCache`; `violation_floor` só em violação). **O custo de um `store` ou `exchange` `seq_cst` frente ao comum, em x86-64 e em ARM, é a verificar por inspeção do código gerado ou por medição**; a medição da nuvem (abaixo) é só ordem de grandeza. **Escritores:** `lot_started` e `gather_active` por quem chama `Gather`: a trás com split, a thread única sem split e, **depois de `Drain`**, a frente (3.3, P0); a thread de fences e as da CPU do jogo **não** chamam; `lot_done` idem, sob o `guard`. **Leitores:** a thread da CPU do jogo (no `CollectObserved`, 2 leituras antes e 1 depois por aviso segurado) e o próprio `Gather` |
| (c) Frequência real | `Gather` por **gerenciador** a cada `InvalidateGPUCache`, vazios inclusive (`core/core.cpp:644-646`, **verificado**); `InvalidateGPUCache` roda no início de cada lista, no fim de cada `SignalFence`, no `OnCPURead` e em outros chamadores (3.3), possivelmente em mais de uma thread (`gpu.cpp:128`). **Sem estimativa por lista ou fence:** o diagnóstico **conta** `invalidate_calls`, `invalidate_empty`, `gather_calls[núcleo]`, `gather_empty[núcleo]`, `gather_overlapped`, `invalidate_thread_switches` | os mesmos contadores, no resumo e no `Report()` |
| (d) Sincronização | nenhuma nova | **as gravações ficam sob o `guard` existente:** o `Gather` do diagnóstico acrescenta, **dentro** da seção do `guard`, uma leitura `seq_cst` e uma gravação `seq_cst` (`lot_done`), sem trava nem espera; o incremento de `lot_started` fica **fora** (antes). Amplia a seção crítica de uma gravação atômica: a ordem de ns, que a medição de 8.3 deve confirmar, **não** a presumo. O `Collect` do diagnóstico não amplia a seção do `guard` (só muda a ordem de memória das operações em `current`) |

**Medição na nuvem (ordem de grandeza, não o custo no FE; x86-64, `-O2`, uma thread):** `Collect` 14 ns contra 146 ns
com `CollectObserved` (por aviso segurado); gather + aplicação de 64 avisos, 142 ns por aviso; `NotePop` 107 ns por faixa
sem registros vivos, 119 ns com 8 e 311 ns com 64. O custo de verdade é o do FE (8.3).

Custo sem uso das outras chamadas (`list`): `MarkWrittenBuffer`, `DmaPusher::Step` (por entrada) e os seis pontos
de `NoteForeignWrite`: uma leitura e um desvio cada. **A medir** (seção 8.3).

## 8. Testes

### 8.1 Integração (Catch2, `src/tests/video_core/list_diag.cpp`)

Contra as classes **reais** onde o código permite; o resto por funções puras que o emulador chama do mesmo jeito
(como `pending_download.cpp` faz com `RetainPendingImageDownloads`). Instanciar um `BufferCache<P>` inteiro com um
runtime falso é grande demais para o T3; as decisões ficam em funções puras que recebem os `RangeSet`.

| Teste | Reais | O que confere |
| --- | --- | --- |
| Aviso em bloco de 64 bytes | `Core::GPUDirtyMemoryManager` (`Collect`, `Gather`), `Common::RangeSet` | O `Collect` de 4 bytes volta do `Gather` como **64 bytes alinhados** (hipótese 7.2 contra o código real) e `blk_gpu_bytes_at_risk` = 60 com 64 bytes da GPU no bloco; sem bytes da GPU, 0 |
| Escrita exata dentro do bloco, junção por página | idem | `at_risk` desconta só as escritas **do mesmo lote**; dois avisos na mesma página depois da junção de `gpu.cpp:138-161`, de **núcleos diferentes**: o conjunto `{(núcleo, lote)}` da faixa final contém os dois |
| **Contraexemplo dos lotes** | `GPUDirtyMemoryManager` real | `Gather` retira o lote A → a CPU registra B no **mesmo bloco** → a aplicação de A: B **não é aposentado**, `apply_overlap_other_lot` = 1, e um pop sobre B depois conta como janela |
| Lote nos três caminhos do `Collect`, **com a identidade de cada aviso** | idem | CAS, `guard` e "já coberto" (`:26-47`). **A referência não é "o endereço aparece no callback"**: com vários avisos no mesmo bloco de 64 bytes, uma ocorrência anterior dos mesmos bytes mascara um lote errado. Os roteiros são **escritos à mão, com a coalescência conhecida** (o mesmo bloco nos lotes 1, 2 e 3 e duas vezes no 4, a segunda "já coberta"), e o lote esperado de cada aviso vem do roteiro (quantos `Gather` vieram antes). Um **mutante** que declara o lote seguinte falha pela identidade em **5 de 5**, e a conferência só por endereço é **enganada 3 vezes** (rodado no harness) |
| Publicação atrasada (3.3.1) | `GPUDirtyMemoryManager` real, livro-razão | pausa entre `s2` e a publicação; pausa entre a publicação e a leitura de `apply_started_lot`; aplicação de L concorrente com cada uma; o mesmo roteiro sem a verificação mostra a janela falsa |
| Violação de P0 (3.3) | idem | dois `Gather` no mesmo gerenciador: `gather_overlapped` = 1 e o pop sobre o lote suspeito conta `pop_over_ambiguous`; aplicação fora de ordem: `apply_out_of_order` |
| Lote ambíguo | idem, com `Gather` parado no meio | `Gather` com `lot_started` incrementado e `lot_done` ainda não: o aviso registrado é ambíguo; nenhuma aposentadoria; o pop sobre ele conta `pop_over_ambiguous` |
| Equivalência | idem | `Collect` e `Gather` `Diag=false` e `Diag=true` produzem **a mesma saída** para sequências aleatórias (o diagnóstico não muda o que o manager entrega) |
| Janela do pop, ordem **registro → pop → aplicação** | `Common::OverlapRangeSet` como `async_downloads`, livro-razão | `pop_over_notice` = 1, tríade registrada |
| Pop **antes** do registro; aplicação **antes** do pop | idem | nenhum evento |
| Pop entre o registro e o `memcpy`; aplicação **antes** do `memcpy` | idem, guest falso | o primeiro dá `sample_different`; o segundo dá `sample_equal` **sem pop**, e o teste documenta que o rótulo **não** distingue (3.5) |
| Aviso entre o `Gather` e a aplicação | `GPUDirtyMemoryManager` real | o pop **conta**: o aviso já não está no manager e continua pendente no livro-razão |
| **Reciclagem, com pontos de parada injetados** (política `Hooks`) | livro-razão | (i) **durante a leitura do payload**: o slot é reciclado entre a leitura e a releitura de `state`: `observation_lost`, nenhum valor usado; (ii) **durante a marcação de `t_pop`**: a CAS falha pela geração, o pop não marca o slot novo, `observation_lost`; (iii) **durante a de `t_apply`**: idem, o slot novo continua pendente; (iv) escrita atrasada de `pop1`/`pop2` de geração velha: o leitor novo vê `unverified`. Cada um **determinístico** (a política bloqueia no ponto e libera o outro lado), não por sorte |
| Anel cheio, expiração | livro-razão | `notice_overflow`, `notice_expired`; nenhum evento falso |
| `ClassifyWords` e a comparação | funções puras | cabeçalho, dado de método e parâmetro de macro; entrada retomada com `dma_state` no meio |
| **Desligado: nada muda** | todos | com `EDEN_LIST_DIAG` desligado os mesmos roteiros dão a mesma saída e **nenhum contador novo** é incrementado (`Shared.list_diag` zerada), `lot_started` e `lot_done` **não** mudam, e nenhuma alocação do livro-razão |
| Concorrência | threads reais | escritor, `Gather`, pop e aplicação em threads; compilado à parte com **TSan** na nuvem (`g++ -std=c++20 -fsanitize=thread`), como os harnesses do split (`docs/back-thread-split-plan.md`, 8.1); ao fim, para cada aviso de lote inequívoco, o lote declarado confere com a saída do `Gather` |

O `DmaPusher` com rasterizador falso e o `puller` reais para a parte `list` são **a confirmar** na implementação
(os testes `native_semaphore.cpp` e `indirect_reset.cpp` podem servir de ponto de partida); se não houver forma
pequena, os casos dirigidos ficam só no harness e o teste de integração cobre `ClassifyWords`, a comparação e a
tabela por página.

### 8.1.1 O que o harness cobre (`tools/list-diag-harness/`, feito e rodado)

**O harness não é mais um modelo: é o código do emulador.** `tools/list-diag-harness/run.sh` compila
`src/tests/video_core/list_diag.cpp` (o mesmo arquivo Catch2 do CMake do emulador) contra os `src/video_core/list_diag/*.h`
**reais**, o `src/core/gpu_dirty_memory_manager.h` **real** (a variante `Diag=true` de verdade) e o `Common::RangeSet`
**real** (Boost). O que fica de fora do `src/` é só um stub de `core/device_memory_manager.h` (o tamanho da página) e o
destino do log. Os testes: equivalência do manager (40 000 passos, comparando o `Collect`/`Gather` de antes, o `Diag=false`
e o `Diag=true`); lote e identidade de cada aviso (com o mutante); ambiguidade nos quatro pontos; o contraexemplo A/B;
"entre o `Gather` e a aplicação"; publicação atrasada (e a variante sem a verificação, que mostra a janela falsa);
reciclagem (leitura do payload, CAS de `t_pop`, CAS de `t_apply`, payload do pop); **fronteira de geração** (65534 a
131072, em todos os caminhos); P0 violada (sobreposição e fora de ordem), com o relatório; junção entre núcleos;
rótulos de amostra; `hit`/`applied`/`never` do log e o limite do log; `blk` (bloco, bytes em risco, ambíguo, sem
rastreio de leitura da CPU, partes isoladas); a chave; **desligado, nada muda, e nada é alocado**; ligado, o caminho de
produção; e o estresse com threads (verdade de referência vinda do `Gather`, sem janela falsa, conservação e um mutante).

**O que ele não faz:** não instancia `BufferCache` (o laço do pop e o topo de `CachedWriteMemory` são conferidos por
compilação no emulador e por leitura, não por execução); o `memcpy` concorrente do guest não existe (as amostras usam um guest
de mentira); o estresse usa blocos únicos por aviso (a repetição de bloco é coberta pelos roteiros).

### 8.2 Verificação na nuvem

`tools/gpu-contract-harness/run.sh` (três builds) e `tools/list-diag-harness/run.sh` (três builds: `-O2`, TSan e
ASan+UBSan, LeakSanitizer ativo e confirmado com um programa que vaza de propósito) e
`g++ -std=c++20 -fsyntax-only -Wall` dos arquivos alterados (método de `docs/handoff-aggressive.md` e
`docs/perf-handoff.md`). A cadeia completa de build do emulador não roda na nuvem.

### 8.3 Plano de teste no FE e no Bananza (usuário, em casa)

Orçamento: `docs/architecture-plan.md`, 12.1 (até 5% de draws/s por item). **`EDEN_BACK_THREAD_STATS` ausente** nas
medições de draws/s, para o diagnóstico não medir a si mesmo. O diagnóstico escreve no log do emulador
(`user\log\eden_log.txt`): as linhas de evento na hora (no máximo 1 descarga por segundo) e o **resumo ao fechar o jogo**
(`GPU::NotifyShutdown`); o bloco periódico por segundo só aparece com `EDEN_BACK_THREAD_STATS=1`, que **não** se usa nas
medições de draws/s.

PowerShell novo a cada rodada, mesmo build, mesmo lugar e mesma câmera:

| Rodada | PowerShell | O que medir |
| --- | --- | --- |
| A (referência) | `.\eden.exe` (sem `EDEN_LIST_DIAG`) | draws/s; repita A duas vezes para o ruído |
| B (T3a) | `$env:EDEN_LIST_DIAG="pop,blk"; .\eden.exe` | draws/s e o log |

Alternar A, B, A, B, **na praça** e **no palácio** do FE, GPU em Fast, o mesmo método de draws/s que você usa hoje.
Anotar também os degraus de FPS. Fechar o jogo pelo menu (para o resumo sair no log) em cada rodada B.

**Bananza**, onde o chão some, GPU em Fast, com o split como está (sem `EDEN_GPU_SPLIT=0`): uma rodada
`$env:EDEN_LIST_DIAG="pop,blk"`, o tempo que for preciso para o chão aparecer ou não, e fechar pelo menu.

**O que deve aparecer no log** (com `EDEN_LIST_DIAG=pop,blk`; sem a variável **nenhuma** linha `[ListDiag]`):
1. na partida do GPU, **uma** linha: `[ListDiag] ligado: partes=pop,blk; resumo ao fechar o jogo e totais a cada 30 s`.
   Se ela não está no log, a variável não chegou ao processo (outro PowerShell, ou o exe não é o build novo);
2. a cada ~30 s, linhas `[ListDiag][total] ...` (os contadores não nulos), para o log ter dados mesmo se o jogo for morto;
3. na hora, `[ListDiag][pop] hit/applied/never` e `[ListDiag][blk]` quando houver ocorrência (pode não haver nenhuma);
4. ao fechar pelo menu, `[ListDiag][summary] ...` (e uma linha por núcleo com as chamadas ao `Gather`).

**O que me mandar** (de `user\log\eden_log.txt`; `Select-String -Path user\log\eden_log.txt -Pattern "ListDiag"`):

1. **Todas** as linhas `[ListDiag][summary]` (os contadores totais e, por núcleo, as chamadas ao `Gather`);
2. **qualquer** linha com `P0`: `precondicao P0 violada` (se existir, o resto do diagnóstico daquela sessão não é confiável);
3. as **primeiras 20** linhas `[ListDiag][pop]` e as **primeiras 20** `[ListDiag][blk]` (são as 32 primeiras de cada tipo, depois uma
   amostra; 20 bastam);
4. os números de draws/s de cada rodada, e o hash do commit do build.

O que cada contador diz e **não** diz está em 4 e 5 e em 9: `pop_over_notice` e `blk_gpu_bytes_at_risk` são **limites
superiores** (aviso registrado, não escrita concluída; bytes em risco, não perdidos), e uma rodada com zero ocorrências
**não** invalida a janela nem os blocos (seção 0).

### 8.4 Medições do T3a (rodada 2, 2026-11-05)

**Rodadas (medido pelo usuário).** Build de `fc22def`. A string de versão do log diz `bold-dirac-tppzm7-32e6e17`, mas
a linha `[ListDiag] ligado ... totais a cada 30 s` só existe a partir de `fc22def` (`list_diag.h:150`; ausente em
`32e6e17`, conferido com `git grep`), e as macros estão na frente nos dois jogos, o que só acontece com `8ee3728`:
a string está velha (cache do CMake). FE: quatro rodadas no **mesmo lugar** (25 s depois de carregar o save,
parado), sem VSync, só `EDEN_BACK_THREAD_STATS=1` nas quatro (diferente do 8.3, mas igual entre elas), mais
`EDEN_LIST_DIAG=pop,blk` em b e d (conferido pela linha `ligado`). Split padrão (`2`), macros na frente em todas.
Nenhuma violação de P0 nas três rodadas com diagnóstico.

**Custo no FE (medido, `Back thread:` dos logs, amostras de 4 s depois da subida):**

| Rodada | Diag | draws/s (média) | Trás ocupada |
| --- | --- | --- | --- |
| a | desligado | 81 577 (5 amostras, 40-56 s) | 86,7% |
| c | desligado | 80 602 (5 amostras, 40-56 s) | 87,2% |
| b | `pop,blk` | 65 137 (5 amostras, 42-58 s) | 78,2% |
| d | `pop,blk` | 64 641 (5 amostras, 44-60 s) | 78,2% |

Desligado 81 090, ligado 64 889: **−20,0%** de draws/s, com ruído de ~1% entre as rodadas iguais. A linha de
base (seção 6 do `handoff-planner.md`, −18%) se confirma no mesmo lugar.

**O custo está distribuído, não só nas threads do jogo (correção da revisão 4; medido, mesmos relatórios).** A thread
de trás fica menos ocupada (86,98% → 78,21%) porque recebe menos trabalho, mas o `other busy` sobe de 15,2% para
20,2% do tempo de parede, com `prepare draw` igual por draw (7,56 → 7,60 µs). Há também uma **perturbação de
comportamento**: os lotes de escritas seguradas passam de ~400/s (27,5 trechos cada, 10,2 páginas) para ~1 040/s
(10,7 trechos, 4,6 páginas; último relatório de cada rodada). O total de trechos por segundo fica parecido (~11 mil). Ou seja, o diagnóstico
**reparte a mesma aplicação em 2,6 vezes mais lotes**. Isso multiplica os custos por lote na thread de trás
(travas, varreduras de `NoteApply`), e as janelas A e B são observadas sob um agrupamento diferente do que existe
sem o diagnóstico. A causa é **hipótese**: escritores mais lentos espalham os avisos no tempo. Os dois lados
(escritor e aplicação) precisam ser medidos de novo.

**Custo do lado do escritor (verificado no código; quanto cada item pesa é hipótese).** São ~555 mil avisos/s
(b: 24,4 mi em 44 s; d: 25,0 mi em 44,7 s), e **80% vão para overflow**:
- `Ledger::Register` (`list_diag/ledger.h:160-210`) examina até `kScan = 8` slots (`:25`) e chama `Expired` em
  cada slot publicado. `Expired` lê o relógio a cada chamada (`:354-358`; `steady_clock::now`, `hooks.h:17-21`).
  Um aviso que vai para overflow pode custar até 8 leituras de relógio, mais 8 cargas `seq_cst`.
- Os contadores são `std::atomic<u64>` adjacentes numa `struct` só (`counters.h:20`, `:36-43`). `notice_registered`,
  `notice_overflow` e `notice_applied` estão na mesma linha de cache e são incrementados pelos seis núcleos do
  jogo e pela thread que aplica: compartilhamento falso a centenas de milhares por segundo.
- Todo aviso faz 3 cargas `seq_cst` do lote (`protocol.h:101-106`) e, se registrado, CAS, `exchange`, `store`
  `seq_cst`, `fetch_or` `seq_cst` no índice de vivos (`ledger.h:341-343`) e mais leituras de relógio.

**Cobertura e magnitude (medido):**

| Medida | b (FE) | d (FE) | Bananza |
| --- | --- | --- | --- |
| Registrados ÷ (registrados + overflow) | 19,7% | 19,5% | 16,6% |
| A: pops sobre aviso / registros / bytes | 32 / 582 / 2 588, todos `sample_equal` | 12 / 533 / 2 300, todos `sample_equal` | 10 / 361 / 2 144, 1 `sample_different` |
| B: aplicações sobre bytes da GPU | 201 272 de 347 917 (58%) | 205 370 de 355 514 (58%) | 67 277 de 74 497 (90%) |
| B: **ambíguas** entre as que cobrem bytes da GPU | 139 975 (70%) | 144 768 (70%) | 49 847 (74%) |
| B: `at_risk` (só não ambíguas) / `not_seen_cpu_read` | 6,70 MB / 96% | 5,97 MB / 96% | 1,38 MB / 99% |
| Aviso → aplicação, média / máximo | 6,0 ms / 630 ms | 6,0 ms / 616 ms | 2,2 ms / 77 ms |
| `InvalidateGPUCache`: chamadas / vazias / trocas de thread | 54 509 / 48% / 1 | 55 655 / 48% / 2 | 146 754 / 90% / 24 299 |

Leitura:
- **O `at_risk` não é confiável, nem como limite (correção da revisão 4, verificado).** `NoteApply`
  (`protocol.h:265-300`) só marca ambíguo um trecho com registro suspeito, de outro lote ou parcial. Um aviso
  perdido em overflow, ou reciclado por expiração antes da aplicação, **não deixa rastro**. Com um registro
  encontrado e outro perdido no mesmo bloco e lote, a lista `exact` fica incompleta. **Sem nenhum registro**, ela
  fica vazia e o trecho inteiro conta como "em risco" (`AccountBlock`, `protocol.h:677`). Isso contradiz a seção 5
  ("nenhum registro (overflow) → ambíguo"): a implementação diverge do desenho. Os 70% ambíguos vêm de
  sobreposição com outros lotes ou com lotes ambíguos (`apply_overlap_other_lot` 74 948,
  `apply_overlap_ambiguous_lot` 129 621 no b), não de overflow. Os 30% "não ambíguos" incluem trechos com avisos
  perdidos. O único limite superior válido é `blk_gpu_bytes_covered` (110 MB no b).
- **A** apareceu poucas vezes, sempre consistente com sobrescrita no FE; com 80% dos avisos recusados, isso **não
  mostra que ele é raro** (revisão 4). O `sample_different` do Bananza (4 B em
  `0x86154b8c`, núcleo 4, pop +260 µs, aplicação +738 µs) só diz que a amostra mudou entre o pop e a aplicação
  (seção 4). Não prova nem descarta o A.
- **Bananza:** a rodada não tem par sem diagnóstico, nem lugar estável. Foi só a carga do save e uns 5 s de chão
  preto, com a releitura de macro desligada (padrão desde `8ee3728`). Serve para cobertura e trocas de thread,
  não para custo. As 24 299 trocas de thread no `InvalidateGPUCache` (contra 1 ou 2 no FE) mostram que a
  convenção da P0 é exercitada de verdade nesse jogo; ela não foi violada.
- O ledger tem 1 024 slots por núcleo (`protocol.h:83`). Com ~555 mil avisos/s e 6 ms de vida média, um núcleo
  que concentre os avisos precisa de ~3 300 slots vivos, e `Register` só examina 8 a partir do cursor: o overflow
  é estrutural, não acaso (hipótese: a distribuição por núcleo não é medida).

**Consequência para o plano:** a próxima mudança do diagnóstico precisa baixar o custo, subir a cobertura e
**declarar a cobertura incompleta** quando ela ocorrer, tudo junto. Aumentar o ledger sem mexer no caminho quente
aumentaria o custo. Escopo do T3a.1: seção 11, revisão 4.

## 9. O que fica de fora e o que o diagnóstico não cobre

- **Correções** (T4): nenhuma direção da seção 12 do plano, nenhuma mudança de política de fence.
- **Escrita concluída** (3.5): o diagnóstico vê avisos **registrados**. A terceira intercalação (aplicação antes do
  `memcpy`) **não** é medida: sem hook depois do `memcpy` (caminho quente de toda escrita da CPU); fica como caso
  de harness proposto (3.5, item 4), e o diagnóstico não a distingue.
- **Lotes ambíguos** (retirada concorrente) e **observações perdidas** (reciclagem, anel cheio, expiração): contados,
  nunca resolvidos por palpite; o resultado é um **limite**, não um número exato.
- **Amostras:** hash de 48 bits, até 64 bytes por interseção; `sample_equal` e `sample_different` não são "perda"
  nem "benigno".
- **`cpu_read_ranges` como história da leitura da CPU:** `not_seen_cpu_read` é "fora do rastreio", não "só no host".
- **Pop do texture cache e do query cache** (a seção 12 do plano diz que não foram analisados): não instrumentados.
- **Outros chamadores de `InvalidateGPUCache`** (`engines/maxwell_3d.cpp:435`, `engines/puller.cpp:339`): contados
  junto com os demais, **não abertos**.
- **Escritas de HLE** (decodificador, VIC, vi), **imagens, render targets e aliases**, P4 sem origem da GPU: não
  vistos pelo filtro nem pela tabela por página; saem no relatório como "não cobertos".
- **OpenGL** e o ramo Android do upload (`buffer_cache.h:2008-2028`).
- **Efeitos de tempo do Vulkan** (barreiras, várias filas, caches da GPU): o diagnóstico não os mede.
- **Mudança da flag durante a sessão:** não suportada (lida uma vez).

## 10. Decisões

**Tomadas pelo usuário (revisão 3) e seguidas no T3a:** (1) T3a (`pop` e `blk`) agora; T3b (`list`) só depois do custo do
T3a medido no FE. (2) `Diag` como parâmetro de modelo. (3) Contadores do lote numa estrutura paralela fora do
gerenciador, alocada quando o diagnóstico liga, uma linha de cache por núcleo; **não foi preciso o plano B dos 40 bytes**.
(4) P0 só vigiada, com o relatório de violação (3.3); nenhum `ASSERT` permanente; a checagem em modo de verificação
(`EDEN_VERIFY`, `docs/architecture-plan.md` 12.2 g) fica **pendente**. (5) Hooks de produtores fora do
`MarkWrittenBuffer`: **T3c**, depois. (6) Limites: 32 por tipo e depois 1 a cada 64, 512 linhas, 16 MiB em voo.

**Em aberto:** o **mecanismo** de correção (T4), que as medições só dimensionam; e o desenho do T3b/T3c, depois do
custo do T3a.

## 11. Revisões

| # | O que a crítica disse | Procede no código? | O que mudou |
| --- | --- | --- | --- |
| 1 | O aviso registrado não é escrita concluída: o hook fica depois do `Collect` e antes do `memcpy`; nomes e logs não podem dizer "escrita concluída"; a limitação afeta o T3a inteiro, não só a terceira intercalação | Sim: `HandleRasterizerWrite` roda em `GetPointerImpl` (`core/memory.cpp:587-590`) e o `memcpy` é do chamador (`:649-653`). Eu só tinha tratado a janela do registro como caso benigno do pop, e não como limite da interpretação | Seção 0 e 3.5; nomes `notice_*` e "aviso registrado" em todo o texto; contadores do pop são limite superior; amostras confundidas pela mesma limitação; sem hook novo, com caso de harness proposto |
| 2 | Endereço e sobreposição não identificam o lote (contraexemplo: o `Gather` retira A, a CPU registra B no mesmo bloco, a aplicação de A encontra B). Propôs `lot_started`/`lot_done` por gerenciador, leituras antes e depois do `Collect`, `(núcleo, lote)` até o `NoteApply`, conjunto para faixas juntadas, `blk` só do mesmo lote | Sim. Contraexemplo confirmado (`:56-82` retira sem aplicar; a aplicação é depois, `gpu.cpp:134-162`). O `gather_epoch` (`:57`) é `relaxed` e não serve. **Duas correções à proposta:** o número do lote tem de ser a ordem das **retiradas** sob o `guard`, não dos incrementos (dois `Gather` concorrentes podem inverter); e a demonstração pede `seq_cst` em `current` e nos contadores (a ordem `acquire`/`release`/`relaxed` do `Collect` e do `Gather` de hoje não prova o "lote `d+1`" no caminho "já coberto") | 3.3: protocolo, ordem de memória, demonstração P1 a P4, passagem do `(núcleo, lote)` com a junção, regra de aposentadoria, ambíguo; `Diag` como parâmetro de modelo (desligado = código de hoje); `blk` só do mesmo lote |
| 3 | O seqlock não basta (expiração de 5 s, leitores marcando): estado atômico com geração e fase, CAS do escritor, payload todo `std::atomic`, `t_pop`/`t_apply` numa palavra com a geração e CAS esperando a geração lida, tentativas limitadas, testes determinísticos com pontos de parada | Sim: eu tinha dito "seqlock" e "slot pendente nunca reaproveitado, exceto por expiração", o que deixa o leitor marcar um slot reciclado. Ajuste: o payload do pop também leva a geração, para a escrita atrasada de um leitor não corromper a geração nova | 3.4 inteira (inclusive a ABA declarada) e os quatro testes de reciclagem em 8.1 |
| 4 | Resultados `sample_equal`, `sample_different`, `unverified` com tamanho da amostra e cobertura, sem "benigno" nem "perda"; `host_only` vira `not_seen_cpu_read` | Sim | Seções 4 e 5, contadores e logs |
| Adendo | Gravações de `lot_*` condicionadas ao diagnóstico; declarar (a) a flag, (b) os contadores (atômicos, ordem, leitores), (c) a frequência real **contada**, (d) a sincronização; o lote tem de acompanhar o `Gather` até a aplicação, inclusive com a junção entre núcleos, sem mudar o caminho desligado | Sim. Verifiquei `core.cpp:644-646` (os 6 gerenciadores, vazios inclusive) e `NUM_CPU_CORES = 6` (eu havia escrito 4 no tamanho do anel). `OnCachedWrites` só tem um chamador (`gpu.cpp:162`); `InvalidateGPUCache` tem outros chamadores que não abri | Seção 7 (tabela a a d) e 3.3 (como o lote chega); anel de 384 KiB; contadores reais das chamadas |
| 5 (revisão 2) | **Exclusão entre `Gather` no mesmo gerenciador:** o `guard` protege só a troca; a iteração e o `clear()` do `front_buffer` ficam fora (`:56-82`); inventário de chamadores e threads, com e sem split; a exclusão vale? escrevê-la como precondição ou registrar a falha preexistente | Sim para o fato do código. O inventário **confirma** a leitura do planejador e a **amplia**: a frente também chama, depois de `Drain`, em `puller.cpp:225,252,259,264,291` (`ReleaseFences` direto), além de `dma_pusher.cpp:176-183`; e o `TickWork` do `Defer` é `gpu_thread.cpp:82`, não `:81`. A thread de fences não chama. A exclusão vale nos caminhos lidos **por convenção do `Drain`, não por trava**; não abri `MaybeStart`/`Stop`, `query_cache.h:586` nem o dispatch de `maxwell_3d.cpp:435` | `docs/gpu-contract-inventory.md`, 7.3 (tabela de chamadores); 3.3: P0 como precondição, `gather_active`, `gather_overlapped`, `apply_out_of_order`, `violation_floor`; o harness respeita P0 e tem um teste que a viola de propósito |
| 6 (revisão 2) | **Publicação atrasada:** o escritor publica o aviso de L depois da aplicação de L, e um pop o acha (janela falsa). Propôs `apply_started_lot` (`seq_cst`) gravado antes da varredura e lido pelo escritor depois de publicar | Sim: o contraexemplo é real (a regra de aposentadoria da 3.3 só vê o que já está publicado quando a aplicação varre). A Dekker com `seq_cst` fecha: ou a varredura vê o registro, ou o escritor vê o lote. **Acréscimo:** entre a publicação e a leitura o registro existe e não está conferido; sem um estado `checked` o pop o contaria. Um valor monotônico basta só por P0 | 3.3.1; `late`/`checked` em `marks`; `pop_over_unchecked`; `notice_late`; testes determinísticos e a variante sem a verificação, que mostra a janela falsa |
| 7 (revisão 2) | A referência do teste de lote ("o endereço aparece no callback") não basta quando vários avisos usam o mesmo bloco | Sim, e o harness mostra o tamanho do buraco: com o mutante "lote seguinte", a conferência por identidade falha em 5 de 5 e a por endereço é enganada 3 vezes | 8.1: roteiros escritos à mão, com a coalescência conhecida e o lote esperado vindo do roteiro; o mutante é parte do teste |
| 8 (revisão 2) | Custo desligado: retirar "nenhuma linha de cache nova em uso" e "mesmas instruções em x86" | Sim: 16 bytes (agora 40) podem deslocar campos ativos; um `store` `seq_cst` pode custar mais que um comum | Seção 7 e 3.3: as duas frases viram "a verificar por inspeção do código gerado ou medição" |
| 9 (achado do harness) | (não veio da revisão) A conservação de fim de vida dos registros falhou por 8 no estresse | Sim, **falha do meu protocolo**: reciclar um slot `publicado` e expirado contava `notice_expired` mesmo quando o registro velho já estava `applied` e ainda não liberado (contado duas vezes: retirado e reciclado) | 3.4, passo 3: o escritor sela o `marks` velho por `exchange` e conta `expired` só se o selo não traz `applied` nem `late` |
| 10 (revisão 3, A) | **Geração truncada:** o `Handle` guarda só `gen16` e o `Free` compara com a geração completa; a partir da 65536 a liberação falha, o registro `late` fica publicado e a conservação quebra. Procurar outros pontos que misturem `gen16` e geração completa | Sim, e achei um segundo: `ReadPop` tratava a etiqueta 0 como "sem payload", então toda geração de 16 bits baixos nulos perdia a amostra. Teste determinístico **antes da correção: 6 falhas** (65536: amostra `unverified`, slot preso `publicado`, conservação; 65537: o `Free` também falha). Outro ponto: o `+7` do teste que montava a etiqueta com 16 bits | 3.4: geração completa no `Handle`, `Tag(g) = g % 65535 + 1` (nunca zero), `Free` pela geração completa. Teste de fronteira (65534, 65535, 65536, 65537, 131071, 131072) nos caminhos normal, tardio (Late + Free), aplicação, pop e expiração, com a conservação dos dois lados. **Depois: 0 falhas**, e a mutação que reintroduz o bug é pega na 65536 |
| 11 (revisão 3, B) | O mesmo código no teste e no emulador: protocolo único em `src/video_core/list_diag/*.h`, `Diag=true` no próprio `gpu_dirty_memory_manager.h`, o harness testa a variante real; `manager.h` só para a equivalência ou sai | Sim: o harness anterior testava uma cópia, e uma divergência entre a cópia e o emulador passaria despercebida | Seção 2 e 8.1.1: headers autocontidos incluídos pelos dois; a equivalência usa o `Collect`/`Gather` antigo (`list_diag_legacy_manager.h`); o modelo e o `manager.h` saíram |
| 12 (revisão 3, C) | P0 violada invalida o diagnóstico da sessão, não só alguns lotes: na primeira ocorrência o relatório diz, em destaque e uma vez no log, que os resultados a partir dali não são confiáveis, e o resumo repete | Sim. O `violation_floor` só tornava ambíguos alguns lotes | 3.3: `CheckViolation` (primeira ocorrência de `gather_overlapped` ou `apply_out_of_order`): uma linha no log, o prefixo `[P0 VIOLATED]` no relatório, o aviso no resumo e um **instantâneo dos contadores** no instante da violação; os contadores continuam. Sem trava corretiva |
| 13 (achado ao implementar) | (não veio da revisão) a varredura do livro-razão por faixa lia 6 × 1024 slots, sob o `buffer_cache.mutex` | Sim, e é custo que perturba justamente o que o diagnóstico mede: a praça tem centenas de faixas por lote | 3.4: índice de slots vivos (bitmap) com a ordem publicar-depois-ligar e a releitura em `Free`; o teste confere o índice contra os slots publicados |
| 14 (revisão 4, sobre `dc4d0a9`) | (1) Com o diagnóstico ligado, um aviso de página não amostrada deve continuar em `CollectT<true>`: misturar com `CollectT<false>` no mesmo gerenciador quebra as premissas da prova, porque os avisos não amostrados também mexem no mesmo `current` e empurram dados para `back_buffer`. (2) Overflow parcial no mesmo bloco e lote deixa `exact` incompleta sem marcar ambíguo; pede teste e mecanismo conservador. (3) Amostra por página, sim; predicado pelo mesmo endereço em todos os hooks, independente do núcleo, com `N` e semente fixos e registrados; resultados observados em primeiro lugar, sem multiplicar tudo por `N`. (4) Recorte do pop por página só no diagnóstico, com o deslocamento do ponteiro, o vínculo `(núcleo, lote)` intacto e contagens separadas (pops originais, fragmentos, fragmentos selecionados). (5) A ociosidade da trás não localiza o custo: o `other busy` sobe de 15,2% para 20,2%. (6) Contadores por núcleo continuam atômicos; conservação só com a execução parada | (1) Sim: `CollectT<true>` só troca as ordens de memória de `current` para `seq_cst` (`gpu_dirty_memory_manager.h:56-85`), e a prova da 3.3 assume essa ordem em todo acesso a `current`. No x86-64, CAS e `exchange` já são instruções com trava nas duas variantes, e a carga `seq_cst` é um `mov`: a economia perdida deve ser pequena (mapeamento padrão, não conferido no binário). (2) Sim, e é **pior**: sem nenhum registro, `exact` fica vazia e o trecho inteiro conta como em risco (`protocol.h:265-300`, `:677`), contra o que a seção 5 diz. Achado meu: a reciclagem por expiração (`ledger.h:166-201`) também perde um registro antes da aplicação dele, e cai no mesmo caso. (3) e (4) Sim. (5) Sim, conferido nos logs; achado meu: o diagnóstico reparte a aplicação em 2,6 vezes mais lotes (8.4). (6) Sim | 8.4 corrigido (custo distribuído, `at_risk` não confiável, A não mostrado como raro). Escopo do T3a.1 abaixo |

**T3a.1 (escopo aprovado pelo revisor, revisão 4; desenho a cargo do codador):**
- **Três caminhos no escritor:** desligado = `CollectT<false>` (como hoje); ligado e página fora da amostra =
  `CollectT<true>`, sem ledger, sem carga do lote, sem contador compartilhado; ligado e página na amostra =
  `CollectObserved` completo.
- **Predicado:** `hash(seed, endereço >> 12) % N == 0`, sobre o mesmo endereço em todos os hooks (o `Gather`
  entrega `PAddr`, que o `InvalidateGPUCache` já usa como `DAddr`, `gpu.cpp:133`), sem depender do núcleo. `N` e a
  semente ficam fixos na sessão: `EDEN_LIST_DIAG_SAMPLE` (padrão 8, `1` = todas as páginas) e
  `EDEN_LIST_DIAG_SEED` (padrão fixo), ambos impressos na linha `ligado` e no resumo.
- **Cobertura incompleta declarada:** todo aviso de página amostrada que **não** chega a um registro aplicável
  (overflow; reciclagem por expiração de registro não encerrado) marca, de forma conservadora, a cobertura do seu
  `(núcleo, lote)` como incompleta. Se o lote do aviso for ambíguo, marca todos os lotes a partir dele, como o
  `violation_floor`. A aplicação de um trecho cujo conjunto `(núcleo, lote)` tenha cobertura incompleta conta
  `blk_incomplete` e **não** afirma `at_risk`. Sem registro nenhum num trecho amostrado com bytes da GPU, a
  cobertura também é incompleta, como a seção 5 já previa. Nunca falso "completo"; falso "incompleto" é aceito
  e contado. A granularidade (por `(núcleo, lote)` ou por `(núcleo, lote, página)`) é decisão do desenho.
- **Pop:** recorte por página só no diagnóstico, com o ponteiro deslocado. Contadores separados:
  `pop_ranges` (originais, como hoje), `pop_fragments`, `pop_fragments_selected`. Os eventos de um pop são
  agrupados pelo pop original.
- **Caminho quente:** uma leitura de relógio por `Register`; contadores do lado do escritor por núcleo (atômicos
  relaxados com um escritor só, alinhados a 64 B), somados no relatório. Protocolo dos lotes, P0 e 3.3 sem
  mudança.
- **Relatório:** valores observados; nenhuma multiplicação por `N`. Os totais a cada 30 s são aproximados; a
  conservação só vale no resumo final, com a execução parada.
- **Testes novos:** mistura de páginas selecionadas e não selecionadas nos três caminhos do `Collect`; overflow
  parcial e total no mesmo bloco e lote; reciclagem por expiração antes da aplicação; recorte de pop atravessando
  páginas selecionadas e excluídas; `N=1` equivalente ao T3a, a não ser pelos contadores novos de cobertura
  incompleta; aviso fora da amostra nunca toca o ledger nem os contadores do lote; conservação na amostra com a
  execução parada.
- **Aceite em casa:** custo ≤5% de draws/s como alvo medido; `other busy` e a linha `Invalidation batches`
  comparados com e sem diagnóstico (a perturbação de 2,6 vezes precisa sumir ou ser explicada); `blk_incomplete`
  baixo nas páginas amostradas. Depois, uma rodada só com `pop` e `N=1` para o A, e sementes diferentes.
- **Fora:** T3a.2 (`blkcmp`, só desenho depois do T3a.1 medido), T3b, taxas separadas por parte.
