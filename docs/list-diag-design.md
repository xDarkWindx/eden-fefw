# T3: build de diagnóstico (`EDEN_LIST_DIAG`), desenho proposto

**Estado: T3a, T3a.1, T3a.1b e T3a.2 implementados e medidos (8.4 a 8.6). O `blkcmp` achou bytes antigos da GPU diferentes do payload que os substituiu, nos dois jogos, numa execução instrumentada; próximo: a confirmação sem a interferência das queries e o desenho do T4; T3b (`list`) e T3c (produtores estrangeiros) não começaram.** Revisão 3 (seção 11, linhas 10 a 13). Escrito em 2026-11-05 sobre `740b932`;
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

### 3.7 T3a.1: amostragem por página, cobertura declarada, caminho quente (desenho, sem código)

Escopo: o bloco "T3a.1" da seção 11 (revisão 4). **Nada do emulador foi escrito.** Não muda: o protocolo dos slots
(3.4), o dos lotes (3.3, 3.3.1), a precondição P0, nem o comportamento com o diagnóstico desligado. Mudam só o que
o escritor faz por aviso, o que a aplicação afirma sobre cobertura, o recorte do pop e a contagem. Graus de certeza
como no resto do documento; números de linha são os de `1d41515`.

#### 3.7.1 Três caminhos no escritor e o predicado

| Estado | O que o escritor faz em `core/memory.cpp:768` (`CollectNotice`, `list_diag.h:105`) |
| --- | --- |
| desligado | `manager.Collect` = `CollectT<false>`, como hoje; a flag é uma leitura relaxada |
| ligado, página **fora** da amostra | `manager.CollectT<true>`; **sem** ledger, **sem** carga de `lot_started`/`lot_done`/`apply_started_lot`, **sem** contador compartilhado. Só `seen` do próprio núcleo (3.7.5) |
| ligado, página **na** amostra | `CollectObserved` completo (3.3) mais o protocolo de cobertura (3.7.3) |

**Predicado:** `Sampled(a) = Mix(seed ^ (a >> 12)) % N == 0`, com `Mix` o finalizador do splitmix64. `N == 1` retorna
`true` sem calcular o hash; `N` potência de 2 usa máscara (o padrão 8); outro `N` usa o `%` de 64 bits (~20 a 40
ciclos por aviso, dito). `N` e `seed` vêm de `EDEN_LIST_DIAG_SAMPLE` (inteiro ≥ 1, padrão 8, teto 2^32; valor
inválido ou zero volta ao padrão e a linha "ligado" diz isso) e `EDEN_LIST_DIAG_SEED` (decimal ou `0x`, padrão 1).
Lidos uma vez, como `g_parts`; só os testes os mudam, com o sistema parado. **O predicado não recebe o núcleo.**

**Onde entra em cada hook, e por que o endereço é o mesmo (verificado):**

| Hook | Arquivo:linha | Endereço que o predicado vê |
| --- | --- | --- |
| Escritor | `list_diag.h:105` (`CollectNotice`), chamado de `core/memory.cpp:768` | `address` do lambda de `ApplyOpOnPointer`: o `d_address` que `ApplyOpOnPAddr` calcula (`core/device_memory_manager.h:78-90`), passado como `PAddr` a `Collect`. **Um DAddr** com tipo `PAddr` (`common_types.h:51-52`: os dois são `u64`) |
| Retirada (`GatherAll`) | `protocol.h:178` (callback), do `GatherT` em `gpu_dirty_memory_manager.h:156` | `a = (transform.address << 11) + offset`, e `transform.address = u32(address >> 11)` (`BuildTransform`, `:209`). Então `a >> 12 == address >> 12` enquanto `address < 2^43`; a conversão `PAddr → DAddr` é só `static_cast` (`gpu.cpp:133`, `:142`) |
| `MakeBatch` | `protocol.h:197` | endereço da faixa **final**, que a junção de `gpu.cpp:143-161` mantém dentro de uma página (`(address >> 12) == (last.first >> 12)`) |
| Aplicação | `buffer_cache.h:145` (`NoteApplyHook`) → `NoteApply`, `protocol.h:248` | `device_addr`, o mesmo `DAddr` da faixa final |
| Pop | `buffer_cache.h:728` (`NotePopHook`) → `NotePop`, `protocol.h:308` | cada fragmento de página de `[start, end)`, em `DAddr` (3.7.4) |

- **Mesma página nos três.** O escritor passa o `DAddr` ao `Collect`; o `Gather` devolve esse mesmo endereço com
  os 11 bits baixos refeitos (a granularidade da transformação é 2 KiB, `page_bits = DEVICE_PAGEBITS - 1`, `:100`).
  Duas transformações de 2 KiB formam uma página de 4 KiB e **recebem o mesmo veredito**, porque o hash usa
  `>> 12`. A conta `address < 2^43` vale com folga para `DAddr` (o espaço é muito menor: **hipótese**, não abri o
  limite do `DeviceMemoryManager`).
- **Independente do núcleo.** Duas threads que escrevem a mesma página, em núcleos diferentes, têm o mesmo
  veredito; a retirada junta os núcleos e a aplicação não precisa saber de qual veio.
- **Escrita não atravessa página (verificado, `core/memory.cpp:268-283`, `:322`, `:337`):** `WalkBlock` entrega
  pedaços dentro de uma página de guest de 4 KiB, e cada pedaço chama `HandleRasterizerWrite`. Logo um `Collect`
  tem uma página de 4 KiB; o veredito é o da página do primeiro byte. **Hipótese lida, não executada:** um pedaço
  que cruze o limite de 2 KiB da transformação faz `BuildTransform` calcular `top_bit > 32` (`:204-212`,
  `CreateMask` com deslocamento negativo): comportamento anterior ao diagnóstico, **não mexo nele**, e o veredito
  continua o mesmo (a página da transformação é a mesma que o `Gather` entrega). Vale um olhar do dono do código.
- **Aliases de dispositivo:** `ApplyOpOnPAddr` chama a operação uma vez por `DAddr` (`:86-89`). Cada `DAddr` é um
  aviso próprio com o seu veredito; nada disso muda.
- **Faixas de aplicação e de pop:** a aplicação só recebe faixas dentro de uma página (acima). O pop recebe
  faixas que cruzam páginas: é o único lugar que recorta (3.7.4).

**Por que `CollectT<true>` fora da amostra não quebra a prova da 3.3.** Ela exige que **todo** acesso a `current`
seja `seq_cst` (P1 a P3, a ordem total `S`). `CollectT<true>` é exatamente isso. O aviso fora da amostra não é
reivindicado, então não precisa das cargas do lote; e **a prova não pergunta quem escreveu** o que `current` ou
`back_buffer` guardam: P3 só usa "a primeira retirada depois da leitura `seq_cst` de `current` vê o que a leitura
viu ou mais". Um aviso não amostrado mexe em `current`/`back_buffer` e a prova continua valendo, desde que **toda**
escrita use a variante `seq_cst`. É por isso que o caminho fora da amostra não pode ser `Collect<false>` (a
revisão 4, item 1). **Custo que sobra (x86-64, mapeamento padrão, não conferido no binário):** a carga é um `mov`
nas duas variantes; o CAS e o `exchange` já têm `lock`; a diferença deve ser pequena. Em ARM a diferença é maior
(`ldar`/`stlr` contra `ldapr`/`stlr`; **a medir**).

#### 3.7.2 O que a aplicação e o pop fazem com páginas fora da amostra

- `NoteApply`: a primeira linha avalia o predicado em `addr >> 12`. Fora da amostra: `apply_ranges_seen++` e
  retorna. **Nada** de varredura, de `blk_*`, nem de leitura de contadores de núcleo. (Os dois contadores da
  aplicação ficam na linha compartilhada de `Counters`: só a thread que aplica os toca, sob o `buffer_cache.mutex`
  que ela já tem.)
- `MakeBatch` só calcula a união de núcleos (hoje `O(finais × pré)`, `protocol.h:197-211`) das faixas finais da
  amostra. `Gathered::pre` continua completo, porque `gpu.cpp:141-144` o usa para montar `cached_writes`: **o que é
  aplicado não muda**.
- Faixa fora da amostra com bytes da GPU: nada é afirmado sobre ela. O relatório diz que `blk` é só da amostra.

#### 3.7.3 Cobertura incompleta declarada

**O que se perde hoje (verificado).** Um aviso amostrado pode **não chegar a um registro aplicável**:
(a) `Register` estoura os 8 slots (`ledger.h:160-210`, `notice_overflow`); (b) um registro publicado, expirado e
não encerrado é **reciclado** por outro (`ledger.h:166-201`, `notice_expired`); (c) o registro é marcado `late` e
liberado pelo escritor (3.3.1): o que a aplicação já varreu não o viu; (d) um registro **ambíguo** é publicado
depois da varredura (**achado do desenho:** `CollectObserved` retorna em `protocol.h:127-130` antes da checagem de
`apply_started_lot`, então o ambíguo não tem proteção contra publicação tardia). Nos quatro, o `exact` do `blk`
fica **sem aquele aviso** e, se há outros registros do mesmo bloco e lote, `AccountBlock` afirma `at_risk` sobre bytes
que a CPU escreveu.

**Correção à revisão 4, item 2 (conferido no código):** "sem nenhum registro o trecho inteiro conta como em risco
(`protocol.h:677`)" **não procede em `1d41515`**: `protocol.h:677` é o `blk_bytes_exact`, e a linha `:695`
(`if (ambiguous || merged_exact.empty())`) já devolve **ambíguo** quando `exact` está vazio. Os 70% ambíguos do 8.4
já incluem esses trechos. O que **procede** é a perda **parcial**: um registro achado e outro perdido no mesmo
bloco e lote deixam `exact` não vazio e incompleto, e `at_risk` é afirmado. É esse o buraco que o T3a.1 fecha.

**Mecanismo: duas palavras por núcleo, escritas só pelo escritor do núcleo** (`WriterShared`, uma linha de 64 B,
fora do `GatherLotState`, que fica como está):

- `open` (0 ou 1): "há um aviso amostrado deste núcleo entre o `Collect` e o fim do registro".
- `incomplete_floor` (monotônico): **todo lote ≤ `incomplete_floor` deste núcleo tem cobertura incompleta.**

**Escritor (`CollectObserved`, os passos novos em negrito):**

1. **`open.store(1, seq_cst)`** (O1).
2. `d = lot_done`, `s = lot_started` (3.3); `CollectT<true>`; `s2 = lot_started`.
3. Inequívoco (`s == d && s2 == s`): lote `L = d + 1`, `hi = L`. Ambíguo: `hi = s2 + 1`.
4. **`now = NowUs()`**, a única leitura do aviso (3.7.5); `Register(..., lot_or_hi, now, ...)`. O registro
   ambíguo passa a guardar **`hi` no campo `lot`** (hoje 0); só `Suspect` o lê e ele consulta `ambiguous` antes
   (`protocol.h:595-598`). Isso muda o sentido de um campo do payload, não o protocolo do slot.
5. **Antes do CAS** que recicla um slot `publicado`, expirado e ainda sem `applied`/`late` (conferido por uma
   leitura de `marks`): `RaiseIncomplete(core, slot.lot)`. Reciclar um registro já encerrado não marca.
6. Estouro dos 8 slots: **`RaiseIncomplete(core, hi)`**. Registro marcado `late` (3.3.1): **`RaiseIncomplete(core, L)`**,
   antes da liberação.
7. **`open.store(0, release)`** (O2).

`RaiseIncomplete(core, h)`: `floor = max(floor, h)`, `store seq_cst`. **Um escritor só por núcleo** (o `sys_core` é
serializado por `sys_core_guard`, `core/memory.cpp:709-717`), então `load` + `store` bastam, sem CAS.

**Aplicação (`NoteApply`, só faixa da amostra):**

1. **ρ:** para cada núcleo `c` do `mask` da faixa, `open_c = open.load(seq_cst)`. **Antes** da varredura.
2. σ: a varredura de hoje, sem mudança (`protocol.h:262-297`).
3. **φ:** para cada `c` do `mask`, `floor_c = incomplete_floor.load(seq_cst)`. **Depois** da varredura.
4. Classificação, por precedência, **exclusiva**: (i) ambíguo, como hoje (`ambiguous || !linked || exact vazio`)
   → `blk_ambiguous`; (ii) senão, **incompleto** se algum `open_c == 1` (`blk_incomplete_open`), ou algum
   `floor_c ≥ lots[c]` (`blk_incomplete_floor`), ou a junção está desligada (`blk_incomplete_nomerge`); (iii) senão
   **afirmado** (`blk_affirmed`) e só então `at_risk` conta. No incompleto, `blk_gpu_bytes_at_risk` **não** sobe e
   `blk_incomplete_gpu_bytes` recebe o que o T3a teria afirmado (a identidade do teste de `N=1`, 3.7.7).

**Proposição (nunca falso "completo").** Se a aplicação de uma faixa `R` da amostra afirma `blk_affirmed`, então todo
aviso amostrado `n` do núcleo `c` com bytes em `R` e lote retirado `X = lots[c]` tem os seus bytes **em `exact`**.
(Não se afirma que o registro foi aposentado: `Retire` pode perder a marca `applied` e o `emplace_back` acontece
assim mesmo, `protocol.h:295-296`; revisão de `9311e7a`.)

*Premissas:* (π1) o único produtor é `CollectNotice` (`grep` em `src/`, fora de testes: nenhum outro `Collect`,
**verificado**; os produtores estrangeiros são o T3c); (π2) as faixas finais de um lote são disjuntas (a junção de
`gpu.cpp:143-161`; com `EDEN_HELD_WRITE_MERGE=0` não vale, e por isso `blk_incomplete_nomerge`); (π3) P0 (se violada,
`Suspect` e o relatório de violação continuam valendo); (π4) um escritor por núcleo, **inclusive o núcleo de sistema**: toda thread que não é de CPU do jogo cai em
`sys_core` (`core/memory.cpp:707-709`; o guard é o de `:709-717` que você citou, hoje `:713-720`), e `HandleRasterizerWrite` toma o `sys_core_guard` (`:713-720`, solto pelo
`SCOPE_EXIT`) em volta de `ApplyOpOnPointer`, cujo lambda chama `CollectNotice` (`:768`). **Toda** atualização de `open`,
`incomplete_floor` e dos `WriterCounters` fica dentro desse caminho serializado (inclusive o laço dos pedaços do D1, que está
dentro de `CollectT`); nenhuma outra thread as escreve.

*Caminho efetivo de cada pedaço (revisão do revisor).* Com o D1 (`docs/gpu-contract-inventory.md` 7.4.1) um aviso `n`
vira um ou mais pedaços `p`, todos dentro de `CollectT<true>`, portanto entre `s` e `s2` e depois de O1 na ordem do
programa. Para cada pedaço, `A_p` é a operação `seq_cst` que o torna visível ao `Gather`, e **não** a primeira leitura de
`current`, porque o laço de CAS pode falhar e repetir depois de um `Gather` ter trocado `current`
(`gpu_dirty_memory_manager.h:67-84`; a repetição relê `current`, e só o desfecho conta):

- **CAS:** o `compare_exchange_weak` que **deu certo** (`:84`);
- **publicação sob o `guard`:** o `exchange` de `current` com o pedaço (`:72-75`; o `emplace_back` no `back_buffer` leva
  o valor antigo, que é de outro aviso e não importa para `n`);
- **já coberto:** a carga `seq_cst` de `current` que viu os bits (`:80-81`).

`E_p` é a primeira retirada (o `exchange` de `current` do `Gather`) depois de `A_p` em `S`; no caso "já coberto" os bits
que a carga viu estão em `current` ou, se um caminho `guard` os moveu, em `back_buffer`, que a mesma retirada leva
(3.3, P3). A aplicação do lote de `E_p` vem depois dela, na mesma thread (`GatherAll` e `OnCachedWrites` na mesma chamada
de `InvalidateGPUCache`, `gpu.cpp:140` e `:182`).

*Prova.* **(a)** Para todo pedaço, na ordem total `S`: `O1 < A_p < E_p < ρ` (O1 precede cada `A_p` pela ordem do programa,
e as retiradas só existem depois de um incremento de `lot_started`). Como `X_p` é o lote de `E_p` e `lots[c]` o do
`Gather` que entregou a faixa, o pedaço de `n` que está em `R` tem `X_p = lots[c]`.

**(b) Caso inequívoco (`s == d`, `s2 == s`): todos os pedaços são do lote `d + 1`.** Retiradas cujo `exchange` precede
`A_p` incrementaram `lot_started` antes dele, portanto antes da leitura de `s2` (`A_p` precede `s2`): como `s2 == s == d`,
não houve incremento entre as leituras, então essas retiradas são as `d` já terminadas (P1). `E_p` é a primeira depois
de `A_p`, logo o seu número é `d + 1`. O mesmo para cada pedaço; um `Gather` que comece entre dois pedaços incrementa
`lot_started` e `s2 != s`: o aviso **não** é inequívoco, vai ao caso (c).

**(c) Caso ambíguo: o lote possível de cada pedaço é `≤ s2 + 1`.** As retiradas anteriores a `E_p` são as anteriores a
`A_p`; cada uma incrementou `lot_started` antes do seu `exchange`, logo antes da leitura de `s2`: são no máximo `s2`.
Então `X_p ≤ s2 + 1`; e `X_p ≥ d + 1` por P1. Os pedaços de um mesmo aviso podem ficar em lotes diferentes dentro de
`[d + 1, s2 + 1]`; `hi = s2 + 1` (passo 3) cobre todos, e o piso sobe até `hi`. Nenhum lote é atribuído ao aviso.

- ρ lê `open` depois de O1 em `S`. Como `open` tem um escritor e O1 é `seq_cst`, ρ devolve o valor de O1 ou de uma
  gravação posterior. **(1)** Se devolve 1 (de O1 ou de um O1 de aviso seguinte), a faixa é incompleta
  (`blk_incomplete_open`): **não é "afirmado"**. **(2)** Se devolve 0, a gravação lida é O2 de `n` ou posterior
  (as de antes de O1 estão excluídas). O2 é `release` (**não** `seq_cst`: a prova não precisa), ρ é `acquire`: tudo que o escritor fez antes de O2 acontece
  antes de σ e de φ. Casos do resultado do registro de `n`, todos terminados **antes** de O2:
  - (2a) **estouro**: `RaiseIncomplete(c, hi)` com `hi ≥ X`; φ lê `floor ≥ X` (monotônico): incompleto.
  - (2b) **registrado, não tardio, inequívoco**: o `state publicado` (`seq_cst`, `ledger.h:204`) e depois o `SetLive`
    (`:205`) vêm antes de O2; ρ (`acquire`) lê o valor de O2 ou posterior, então tudo isso acontece antes de σ. Há
    **três** formas de σ não enxergar o registro, e as três exigem uma reciclagem do slot, que levanta o piso antes:
    1. *O `Read` falha.* `Read` faz duas cargas `seq_cst` de `state` (`ledger.h:216` e `:237`, com a cerca entre elas).
       Se a primeira não vê `publicado` na geração de `n`, ou a segunda difere da primeira (reciclagem **durante** o
       `Read`: `observation_lost`), o valor lido é o de uma escrita de `state` **posterior** à de `n`. A única escrita
       posterior possível sem passar por uma aplicação deste lote é a reciclagem por `Register` (passo 5: piso,
       **depois** o CAS que escreve `state`; as escritas seguintes do escritor, `kWriting` e a nova publicação, são
       `seq_cst` e vêm depois do piso na ordem do programa dele). Uma carga `seq_cst` que lê essa escrita sincroniza com
       ela: o piso acontece antes de φ (σ e φ são da mesma thread). A liberação pelo escritor (`protocol.h:151`) só
       existe para `late`, que é o caso (2c).
    2. *Rejeição posterior por geração em `marks`.* O `Read` passou, mas a reciclagem veio depois dele: `Marks(slot)`
       (`protocol.h:281`, `seq_cst`) devolve a etiqueta nova (o `exchange` de `marks`, `ledger.h:192`, é posterior ao
       piso) e `(m & 0xffff) != v.tag()` descarta o registro. Mesma conclusão: φ lê o piso. Se o que falha é a marca
       `applied` do `Retire` (`Mark` perdida, `observation_lost`), o registro **já estava em `exact`** (o `emplace_back`
       vem depois do `Retire` e não depende do resultado); o aviso novo, que reciclou o slot, é outro aviso.
    3. *O índice de vivos não mostra o slot.* `ForEachLive` só visita bits ligados. O `SetLive` de `n` vem **depois** do
       `state publicado` e antes de O2, e o bit só é apagado por `ClearLive`, que **relê `state` depois do `fetch_and`**
       (`ledger.h:347-350`) e religa o bit se o slot está `publicado`. Um `ClearLive` de um registro **anterior** do
       mesmo slot que se intercale com o `Register` de `n`: se o `fetch_and` vem antes do `SetLive` de `n`, o bit é
       religado por ele; se vem depois, a releitura (posterior ao `state publicado` de `n` em `S`) vê `publicado` e
       religa. O bit só pode faltar se o slot deixou de estar publicado, que é a reciclagem (casos 1 e 2). Quem chama
       `Free`: a **aplicação** (`protocol.h:644`, sob o `buffer_cache.mutex`), que termina antes da aplicação seguinte
       começar (P0), e o **escritor** (`protocol.h:151`, só `late`), que termina antes do próximo `Register` daquele
       núcleo, por π4 (mesma thread, e o `sys_core_guard` cobre o núcleo de sistema). Logo o único `ClearLive` que pode se
       sobrepor ao `Register` de `n` é o da aplicação anterior, tratado acima.
    Se σ o vê publicado: o registro tem `lot == X`, `core == c` e está contido na faixa (um que só cruza a borda vira
    `apply_partial`, ambíguo, T3a): entra em `exact`.
  - (2c) **registrado e `late`**: o piso subiu antes da liberação (passo 6): φ lê `floor ≥ X`: incompleto.
  - (2d) **registrado ambíguo**: publicado antes de O2, σ o vê (ou a reciclagem, como em 2b), e o vê como `Suspect`:
    `ambiguous`, que tem precedência (i). O piso cobre `hi ≥ X`.
  Em nenhum caso a faixa sai "afirmada" com `n` fora de `exact`. ∎

*Overflow de lote ambíguo.* É o passo 6 com `hi = s2 + 1`: marca os lotes de `d + 1` a `s2 + 1` (e os `≤ d + 1`
que ainda não foram aplicados, o excesso abaixo). **Não marca os seguintes:** o lote `s2 + 2` ou maior não pode
conter o aviso (limite acima), então o piso **não** envenena o futuro. (A frase do escopo "marca todos os lotes a
partir dele, como o `violation_floor`" fica assim: o `violation_floor` também é um teto, `lote ≤ piso`; "dali em
diante" sem teto cobriria o núcleo para sempre e zeraria a cobertura no primeiro aviso ambíguo.)

*Expiração.* Passo 5: o registro velho é reciclado **depois** de o piso subir (antes do CAS), então nenhuma
varredura vê "slot trocado" sem enxergar o piso. Só um registro **não encerrado** marca; um já `applied`/`late`
não. **Observação (medida só em parte):** registros ambíguos nunca são aposentados (T3a, "o não aposentado expira")
e expiram em 5 s; o piso subiria para um lote já aplicado há 5 s e não muda nada (monotônico, antigo). O que a
expiração marca de fato é o caso raro de a aplicação do lote do registro ainda não ter acontecido em 5 s.

**Falso "incompleto" aceito, e contado.** (i) O piso é um teto: o lote anterior a `d + 1`, se retirado e ainda não
aplicado (no máximo um, por P0), é marcado sem necessidade. (ii) `open == 1` na hora da leitura (um aviso no meio do
registro, ~centenas de ns): se a fração do tempo com `open` ligado em um núcleo for ~0,2% (**estimativa**: ~12 mil
avisos amostrados por segundo por núcleo no `N = 8`, ~200 ns cada), é ~0,2% das faixas. (iii) a faixa fica
incompleta por um estouro em **outra página** do mesmo (núcleo, lote) (a granularidade, abaixo). Tudo isso vira
`blk_incomplete_*`; nunca "afirmado".

**Granularidade: (núcleo, lote), recomendada; (núcleo, lote, página) descrita e adiada.**

| | (núcleo, lote) | (núcleo, lote, página) |
| --- | --- | --- |
| Estado por núcleo | 2 palavras: `open`, `incomplete_floor` | tabela de `K` entradas (página, teto) endereçada pela página, mais `open` por núcleo |
| Escritor, na falha | 1 `store` | 1 `store` na tabela; **colisão com entrada viva de outra página cai no (núcleo, lote)** (senão perderia informação e daria falso "completo") |
| Aplicação | ≤ 6 cargas de `open` e ≤ 6 de piso | o mesmo mais a busca na tabela por faixa |
| Prova | acima | refazer 2b a 2d com a página, mais o caso da colisão |
| Falso "incompleto" | uma falha marca todas as faixas amostradas do (núcleo, lote) | só a página da falha (e as da mesma colisão) |
| Ganho esperado | `N = 8`: o lote tem em média **4,6 páginas** (8.4, medido, com diagnóstico), então ~0,6 página amostrada por lote: a diferença entre as duas é **pequena**. `N = 1`: quase toda página tem falha (80% de estouro, medido), a página não ajuda | Aparece com `N = 2` a `4` e taxa de falha baixa mas não nula |

**Decisão proposta:** (núcleo, lote). Critério para trocar, medido em casa: `blk_incomplete_floor ÷
(blk_applies_over_gpu)` acima de ~10% em `N = 2` a `4` **e** `notice_overflow + cov_expired` baixos; senão a página
não compra nada.

**Pop e cobertura:** o recorte não depende da cobertura. Mas `pop_over_notice` em uma página amostrada com
`notice_overflow` ou `cov_expired` diferente de zero é **um piso** (avisos perdidos não aparecem). O resumo diz isso
com os dois contadores ao lado.

#### 3.7.4 Pop: recorte por página só dentro do diagnóstico

`PopAsyncBuffers` (`buffer_cache.h:725-729`) chama `NotePopHook(start, end, &read_mapped_memory[start - device_addr])`
e, em seguida, `WriteBlockUnsafe(start, ..., end - start)`: **a escrita do download não muda**. Dentro de
`NotePop` (`protocol.h:308`):

```
pop_ranges++                                   // originais, como hoje
first = start >> 12;  last = (end - 1) >> 12
pop_fragments += last - first + 1
selecionados = N == 1 ? (last - first + 1) : #{p em first..last : Sampled(p << 12)}
pop_fragments_selected += selecionados
se selecionados == 0: retorna                   // não varre o ledger
varre o ledger UMA vez sobre [start, end) (como hoje); cada registro cai no fragmento
  da sua página (addr >> 12). Hash e amostra: bytes + (lo - start), lo = max(start, v.addr),
  a mesma conta de hoje; o ponteiro deslocado de cada fragmento é bytes + (frag_lo - start).
agrega por pop original: pop_over_notice (+1 se algum fragmento atingiu), pop_over_notice_bytes
  (soma), pop_over_notice_fragments (fragmentos selecionados atingidos)
```

- **Por que uma varredura e não uma por fragmento.** `ForEachLive` custa o número de registros vivos, e os
  registros só existem em páginas selecionadas (o escritor não registra as outras). Varrer a faixa inteira dá o
  mesmo resultado que varrer fragmento por fragmento e custa uma vez; o recorte serve para **contar** e para
  **saltar** a varredura quando nenhum fragmento é da amostra. A equivalência (cada registro cabe em uma página)
  é teste (3.7.7, linha 8), contra uma referência que varre por fragmento.
- **Vínculo (núcleo, lote) intacto:** o pop não usa lote (usa `Suspect` e `checked`); nada muda.
- **Eventos agrupados pelo pop original:** o evento `PopHit` continua um por registro atingido, com `v[0], v[1]` =
  o `[start, end)` **original** e `v[2]` = os bytes da interseção; ganha `v[4]` = fragmentos do pop e `v[5]` =
  fragmentos selecionados, para o leitor agrupar linhas do mesmo pop sem recorrer ao tempo.
- **Duas taxas, dois denominadores:** `pop_over_notice ÷ pop_ranges` (por pop original, **viesada** pelo número
  de páginas do pop) e `pop_over_notice_fragments ÷ pop_fragments_selected` (por página amostrada: a taxa limpa).
  O relatório imprime as duas e a diferença. **Sem multiplicar por `N`.**
- `N = 1`: todos os fragmentos selecionados, uma varredura sobre a faixa inteira: **o `NotePop` de hoje**, mais os
  contadores novos.

#### 3.7.5 Caminho quente

**Relógio: uma leitura por aviso** (hoje, até 9 por `Register`: `ledger.h:355` em cada slot publicado examinado,
`:185` para `t_write`, `:250` em `Mark`, e `protocol.h:137` na saída). Passa a ser `now`, lido uma vez depois do
`Collect` e passado a `Register(…, now, …)`: `Expired(s, now)`, `t_write = now`, e o `Mark` de `Checked`/`Late`
recebe `t_write` e **não lê o relógio** (só `Popped` e `Applied` usam a hora, para a largura; ficam como estão,
na thread da aplicação/pop, sob o mutex). Efeito na largura: `t_write` fica alguns ns antes da publicação.

**Contadores do lado do escritor, por núcleo (`WriterCounters`, `alignas(64)`, 8 palavras = 1 linha):** `seen`,
`sampled`, `registered`, `ambiguous`, `late`, `overflow`, `expired`, `floor_raises`. `std::atomic<u64>` **relaxado**,
com um escritor por núcleo: o incremento é `store(load() + 1)`, sem RMW (sem `lock`). Se a premissa π4 falhar, o
incremento perde contagens (nunca corrompe) e a conservação do resumo acusa. Saem da `struct Counters`
compartilhada (hoje `notice_registered`, `notice_overflow` etc. na mesma linha, incrementados pelos seis núcleos e
pela aplicação: `counters.h:20`, `:36-43`); ficam nela só os contadores da aplicação, do pop e do `blk`, que
são incrementados em uma thread por vez, sob o `buffer_cache.mutex`, e `observation_lost` (rara, várias threads).
`open` e `incomplete_floor` ficam em **outra** linha (`WriterShared`), porque a aplicação as lê e não deve
invalidar a linha dos contadores do escritor.

**Fora da amostra, o que o escritor faz:** a flag, o hash, `seen` do próprio núcleo (um `load`+`store` na própria
linha), `CollectT<true>`. **Nenhum** outro dado compartilhado, nenhuma leitura de relógio. (`seen` é opcional: dá a
fração observada de **avisos** selecionados (`sampled/seen`), que o resumo mostra ao lado de 1/N **sem** exigir
proximidade em roteiros com páginas quentes repetidas (a fração de avisos segue a distribuição de páginas do jogo, não a de
páginas distintas). Se o revisor o quiser fora, a verificação do
predicado passa a ser só o teste 15.)

**Aviso amostrado:** a mais, `open` (um `store seq_cst`, em x86 um `xchg`, e um `store release`, um `mov`); 3
cargas `seq_cst` do lote como hoje; `Register` com uma leitura de relógio.

**Soma no relatório:** `ReportLines` e `SummaryLines` leem cada `WriterCounters[c]` (relaxado) e somam. Com a
execução em andamento a soma **não é um corte consistente**: os campos são lidos em instantes diferentes. Por isso
as linhas `[total]` de 30 s e o relatório por segundo dizem "(aproximado)"; **a conservação só vale no resumo final
com a execução parada**, e o resumo imprime as identidades com o resíduo e essa ressalva.

#### 3.7.6 Relatório: campos novos

- **Linha "ligado"** (`list_diag.h:149`): `[ListDiag] ligado: partes=pop,blk; amostra=1/8 de paginas de 4 KiB
  (semente=0x1, hash=splitmix64, N valido); resumo ao fechar o jogo e totais a cada 30 s (aproximados: lidos com a
  execucao em andamento)`. Com valor inválido: `N=8 (padrao: EDEN_LIST_DIAG_SAMPLE='abc' invalido)`.
- **Linhas `[total]`** (a cada 30 s), grupo novo `sample`: `seen`, `sampled` (e `sampled/seen`, a fração de **avisos** selecionados, **não** de páginas distintas; observada, ao lado de
  `1/N` só como referência), `apply_ranges_seen`, `apply_ranges_selected`, `pop_fragments`, `pop_fragments_selected`,
  `pop_over_notice_fragments`. No grupo `notices`: os de hoje (somados) e `floor_raises`. No grupo `blk`:
  `blk_incomplete` (`_open`, `_floor`, `_nomerge`), `blk_incomplete_gpu_bytes`, `blk_affirmed`,
  `blk_ambiguous_no_record` (dentro de `blk_ambiguous`: o `exact` vazio, a causa que a revisão 4 queria separar).
  Prefixo `(aprox.)` em todas.
- **Resumo final:** os mesmos, mais (1) `amostra`: `N`, semente, `seen`, `sampled`, fração de **avisos** selecionados (`sampled/seen`); (2) `cobertura`:
  `notice_overflow`, `cov_expired`, `late`, `ambiguous` e `floor_raises` lado a lado com `blk_incomplete*`, e a frase
  "pop_over_notice nas paginas amostradas e um piso enquanto estes forem diferentes de zero"; (3) `conservacao (so com
  a execucao parada)`: `sampled = registered + overflow`; `registered = applied + late + expired + pendentes`;
  `blk_applies = apply_ranges_selected`; `blk_applies_over_gpu = blk_ambiguous + blk_incomplete + blk_affirmed`;
  `pop_ranges ≤ pop_fragments`, `pop_fragments_selected ≤ pop_fragments`; cada uma com os dois lados e a diferença;
  (4) `escala: nenhuma (valores observados; a amostra e 1/N das paginas, nao e multiplicada)`.

#### 3.7.7 Testes novos do harness (`tools/list-diag-harness/`, mesmo código do emulador)

Os pontos de parada novos (`Hooks::At`): `w.after_open`, `w.after_overflow`, `w.before_open_clear`,
`r.before_reclaim_cas`, `a.after_open_read`, `a.after_sweep`. Páginas "da amostra" e "fora" são escolhidas pelo teste
chamando o predicado real com `N` e semente fixos.

| # | Teste | Confere |
| --- | --- | --- |
| 1 | **Mistura nos três caminhos do `Collect`** (CAS, `guard`, "já coberto"), páginas da amostra e fora no **mesmo** gerenciador e lote | a saída do `Gather` é igual à do `Collect` de antes para a mesma sequência (equivalência, 40 000 passos); o lote de cada aviso **amostrado** é o do roteiro (identidade, com o mutante "lote seguinte"); os fora da amostra não aparecem em nenhum slot |
| 2 | **Aviso fora da amostra nunca toca o ledger nem os contadores do lote** | M avisos fora: os 6 anéis byte a byte iguais (todo `state` zero), `GatherLotState` igual, **todo** campo de `Counters` zero, índice de vivos zero, leituras de relógio = 0; só `seen[núcleo]` subiu |
| 3 | **Overflow parcial e total no mesmo bloco e lote** (anel pequeno, `kScan` curto) | parcial: a, c registrados e b estourado no bloco: `exact = {a, c}`, `blk_incomplete_floor = 1`, `blk_gpu_bytes_at_risk = 0`, `blk_incomplete_gpu_bytes` = o que o T3a afirmaria. Total: `exact` vazio, `blk_ambiguous_no_record = 1`. Controle negativo `NaiveNoCoverage`: afirma `at_risk` sobre os bytes de b |
| 4 | **Overflow com lote ambíguo** (`Gather` parado no meio, como o teste de ambiguidade) | piso = `s2 + 1`; as aplicações dos lotes `d + 1` a `s2 + 1` são incompletas; a do lote `s2 + 2` é **completa** (o piso não envenena o futuro) |
| 5 | **Expiração antes da aplicação** (relógio manual) | registro velho não encerrado, o aviso seguinte o recicla: `cov_expired = 1`, a aplicação do lote dele é incompleta; com o registro **já aplicado**, nada marca; parada em `r.before_reclaim_cas`: a aplicação rodando **entre** a subida do piso e o CAS nunca sai completa (nas duas ordens) |
| 6 | **`open`, determinístico** | escritor parado depois do `Collect` (`w.after_open`): o `Gather` retira e a aplicação roda: `blk_incomplete_open`, sem `at_risk`; solto, o lote seguinte é completo. Escritor parado antes de O2: idem. Mutante `NaiveNoOpenFlag`: a **falsa completude aparece** (o teste é sensível) |
| 7 | **Publicação tardia e ambíguo tardio** (3.3.1 e o achado 3.7.3.d) | o aviso é publicado depois da aplicação: `late`, piso sobe, a aplicação seguinte do lote é incompleta; o ambíguo publicado depois da varredura: `open` ou piso o declara; sem os dois (mutante) o trecho sai "afirmado" |
| 8 | **Pop atravessando páginas da amostra e fora** | faixa de 4 páginas, 2 da amostra: `pop_ranges = 1`, `pop_fragments = 4`, `pop_fragments_selected = 2`, `pop_over_notice = 1` (não 2), `pop_over_notice_fragments = 2`; bytes distintos por posição: o hash do fragmento sai de `bytes + (lo - start)` (o ponteiro errado dá `sample_different` com guest igual); faixa que começa e termina no meio de página; a varredura única é igual a uma varredura por fragmento de referência |
| 9 | **`N = 1` equivalente ao T3a** | o mesmo roteiro aleatório contra uma cópia congelada do protocolo do T3a (`1d41515`) **que também tem o D1** (o gerenciador com o recorte de 2 KiB, chave ligada nos dois lados): assim a diferença que sobra é só a do diagnóstico, e a correção do gerenciador é separada (um teste à parte compara o D1 com a chave em 0 e em 1, 7.4.1). Todos os contadores e eventos iguais **exceto** os novos; `blk_gpu_bytes_at_risk(T3a) = blk_gpu_bytes_at_risk(T3a.1) + blk_incomplete_gpu_bytes` |
| 10 | **Conservação na amostra com a execução parada** | 6 escritores, `Gather`/aplicação e pop em threads, `N` = 1, 8 e 64: ao parar, `sampled = registered + overflow` (por núcleo e no total), `registered = applied + late + expired + pendentes`, e as demais identidades de 3.7.6; a fração `sampled/seen` (de **avisos**) só é comparada com 1/N num roteiro de **páginas distintas, uma vez cada**, escolhidas pelo teste (a menos de 4σ de 1/N); com páginas quentes repetidas ela não tem de se aproximar de 1/N; **nenhum falso completo**: para cada faixa afirmada, o conjunto de avisos da referência (a saída do `Gather` com a identidade de cada aviso) está contido em `exact`; os mutantes dos testes 3 e 6 produzem ao menos um falso completo aqui. Também sob **TSan** |
| 11 | **Contadores por núcleo** | `static_assert(sizeof == alignof == 64)` de `WriterCounters` e `WriterShared`; `Counters` não tem mais os campos do escritor; a soma por núcleo = o que o roteiro mandou |
| 12 | **Uma leitura de relógio por `Register`** | anel com 8 slots publicados e não expirados: estouro com 1 leitura (hoje 9); registro normal: 1 |
| 13 | **Desligado: nada muda, nada é alocado** | os roteiros de hoje; `WriterCounters`/`WriterShared` não alocados; o caminho é `CollectT<false>` |
| 14 | **`N` e semente** | valores inválidos voltam ao padrão e a linha "ligado" diz; semente em decimal e hexadecimal |
| 15 | **O predicado é um só** | `Sampled(a)` do escritor = o da faixa que o `Gather` real devolve (a ida e volta `u32(a >> 11)`) = o da aplicação = o de cada fragmento do pop, para endereços aleatórios inclusive as duas metades de 2 KiB de uma página; **independe do núcleo**; uniformidade (qui-quadrado) para `N` = 2, 8, 64 **sobre páginas distintas, uma vez cada** (distribuição controlada pelo teste, não um roteiro com páginas quentes repetidas) |
| 16 | **Reciclagem determinística durante o `Read`** (entre as duas cargas de `state`, `ledger.h:216` e `:237`) | ponto de parada `r.between_state_loads`: um `Register` recicla o slot do registro de `n` (expirado, relógio manual) enquanto a aplicação está entre as cargas: `observation_lost = 1`, o piso subiu antes, a faixa sai **incompleta**, nunca afirmada; o mesmo com a reciclagem **depois** do `Read` e antes de `Marks` (rejeição por geração); mutante sem o piso antes do CAS: a falsa completude aparece |
| 17 | **`Gather` entre os pedaços de 2 KiB de um aviso amostrado** (D1 ligado) | parada em `c.between_pieces` (7.4.1): `s2 != s`, o aviso é ambíguo com `hi = s2 + 1`, os dois lotes possíveis são incompletos/ambíguos e nenhum afirma `at_risk`; sem `Gather` no meio, os dois pedaços são do lote `d + 1` e o aviso é inequívoco; **o aviso dividido entre lotes nunca recebe um lote só** |
| 18 | **Índice de vivos contra um `ClearLive` antigo** | `Free` de um registro anterior do slot parado entre o `fetch_and` e a releitura de `state` enquanto `Register` publica `n`: ao fim o bit está ligado e a varredura visita `n` (as duas ordens do `fetch_and` e do `SetLive`) |

#### 3.7.8 Os 2,6 vezes mais lotes de aplicação com o diagnóstico ligado (8.4): hipóteses lidas no código

Fato medido (8.4): sem o diagnóstico ~400 lotes/s (27,5 trechos, 10,2 páginas); com ele ~1 040/s (10,7 trechos, 4,6
páginas); trechos por segundo parecidos (~11 mil). O contador do lote é `OnCachedWrites` com dados
(`vk_rasterizer.cpp:959`). **Só hipóteses; nada foi tentado.**

1. **O `buffer_cache.mutex` fica mais tempo ocupado pela aplicação, e o escritor espera nele.** `NoteApply` varre o
   ledger **por faixa**, sob o mutex (`buffer_cache.h:145`, dentro de `CachedWriteMemory`); com ~20% dos avisos
   registrados e vida de 6 ms, há centenas de registros vivos (**estimativa**, não medida) e dezenas de faixas por
   lote. O escritor, para uma página que ainda não "lembra", chama `OnCPUWrite`, que toma o **mesmo** mutex
   (`vk_rasterizer.cpp:998`). Mais espera na CPU do jogo muda quando os avisos chegam em relação às chamadas de
   `InvalidateGPUCache`.
2. **Sobreposição de rajadas.** As chamadas vêm da thread de trás e das listas e fences
   (`gpu_thread.cpp:79,106`, `fence_manager.h:139`), **não** das escritas; um lote tem dados se alguma escrita
   caiu entre duas chamadas. Escritas mais lentas alongam as rajadas, e mais chamadas caem dentro delas: lotes
   **mais frequentes e menores**, com o total de trechos igual. Isso bate com o formato do que foi medido (2,6×
   mais lotes, 2,6× menos trechos por lote) **e é só uma leitura**.
3. **A taxa de chamadas sem o diagnóstico não é conhecida.** `invalidate_calls` e `invalidate_empty` existem só com o
   diagnóstico ligado (`counters.h`, `protocol.h:163-195`); o `BackStats` não conta chamadas, só lotes com dados.
   Os 54 509 chamadas e 48% vazias do 8.4 são totais da sessão, os ~1 040 lotes/s são do último intervalo: **não
   se comparam**. Sem a taxa de chamadas sem o diagnóstico, não se separa "mais chamadas" de "mais chamadas com dados".
4. **Descartadas pela direção (lido):** o custo de `GatherAll` e de `MakeBatch` (6 `GatherT<true>`, 2 `fetch_add`
   `seq_cst`, o `std::function` por núcleo, `MakeBatch` em `O(finais × pré)`) alonga a chamada e deveria **juntar**
   mais avisos por lote, o contrário do medido. O `gather_epoch` continua incrementado nas duas variantes
   (`gpu_dirty_memory_manager.h:126`), então o "esquecimento" de páginas lembradas pelo escritor
   (`core/memory.cpp`, `ForgetBefore`) não difere.

**O que o T3a.1 muda:** o escritor perde, em 7/8 das páginas (`N = 8`), tudo menos o hash e o `CollectT<true>`; nas
amostradas, 1 leitura de relógio em vez de até 9 e sem linha compartilhada; a aplicação varre `1/N` das faixas e
cada varredura vê `1/N` dos registros vivos (custo da varredura ~`1/N²`, **estimativa**), o que ataca (1). Se com
`N = 8` os lotes/s voltarem para ~400, a causa estava no custo por aviso ou na varredura sob o mutex. Se ficarem em
~1 040, a causa está no que o T3a.1 **não** muda (`CollectT<true>` ou o `Gather`/`MakeBatch`/`BeginApply` de cada
chamada). **Experimento que separa as duas, só com variável de ambiente:** `EDEN_LIST_DIAG_SAMPLE` enorme (2^20:
quase nada amostrado) deixa só o custo fixo; se a perturbação persiste, é do caminho fora da amostra. Para (3),
proponho (**fora do escopo aprovado, a decidir**) dois contadores só no `BackStats`, sem o diagnóstico:
chamadas de `InvalidateGPUCache` e chamadas vazias.

#### 3.7.10 Notas da implementação (`c700e86`)

Nada do protocolo dos slots, dos lotes ou da P0 mudou. Escolhas onde o texto deixava margem:
- `cov_expired` dos textos é o contador `notice_expired` (a reciclagem de registro não encerrado); não há um segundo.
- O piso é um teto sobre **lotes** (`lote <= piso`): ao subir para `hi`, os lotes anteriores ainda não aplicados também ficam
  incompletos. Os testes 3 e 4 mostram os dois efeitos (um estouro declara incompleto o lote inteiro do núcleo, mesmo o bloco
  que tem registro; o lote `s2 + 2` fica completo).
- `Counters` perdeu os campos do escritor; `Protocol::Sums()` os soma e `Snapshot()` junta os dois grupos para o relatório e o
  instantâneo da violação. As leituras com a execução em andamento levam o prefixo `(aprox.)`.
- Em `ledger.h`: `Register` recebe `now` (a única leitura de relógio) e os contadores/`WriterShared` do núcleo; `Mark` só lê o
  relógio para `Popped` e `Applied`. Pontos de parada novos: `w.after_open`, `w.after_overflow`, `w.before_open_clear`,
  `r.before_reclaim_cas`, `r.between_state_loads`, `a.after_open_read`, `a.after_sweep`, `l.before_clear`, `l.after_clear`.
- Mutantes (só nos testes): `NaiveNoCoverage` (nunca sobe o piso) e `NaiveNoOpenFlag` (nunca liga `open`).
  `LIST_DIAG_FORCE_MUTANT=coverage|open` liga um em todos os testes: o primeiro derruba 6 dos 52 casos, o segundo 2.
- Teste 9: `src/tests/video_core/list_diag_t3a_frozen.h` é a cópia congelada do T3a (`1d41515`) em `namespace T3aFrozen`; ela usa
  o mesmo gerenciador real, então os dois lados têm o D1.
- **Conferência do planejador (`6598a02`):** os três builds reproduzidos na nuvem (112 701 asserções em 52 casos, sem falha).
  Mutantes no binário `-O2`, três rodadas cada: `coverage` derruba sempre os mesmos 6 casos (30 asserções, número que varia
  com o estresse); `open` derruba sempre os testes 6 e 7, e o teste 10 (`list_diag.cpp:2053`/`:2058`) também o pega em 2 de 3
  rodadas. É sensibilidade a mais do estresse, não confiável, como o codador disse.
- T3a.1b (o cursor de `Register` avança `kScan` no estouro) implementado em `5e94a57`; o teste "T3a.1b" cobre o caso.
- Teste 13 é só "o contador do escritor não se mexe com o diagnóstico desligado"; o resto do desligado já era coberto por
  "off, nothing changes".

#### 3.7.9 O que não muda e o que fica em aberto

- Não muda: protocolo dos slots e dos lotes; P0; 3.3 e 3.3.1; o que é aplicado; o desligado. A única mudança de
  **sentido** é o campo `lot` do registro ambíguo (guarda `hi`; 3.7.3, passo 4).
- Em aberto: (a) o `BuildTransform` com escrita que cruza 2 KiB (3.7.1): **confirmado** pelo planejador (GCC, UBSan), bug preexistente, `docs/gpu-contract-inventory.md` 7.4, item D1; (b) o custo do `seq_cst` em ARM;
  (c) a granularidade por página, só se a medição pedir; (d) os contadores do `BackStats` do item 3 acima: **feitos no D1** (`InvalidateGPUCache` total, vazias e com dados);
  (e) `EDEN_HELD_WRITE_MERGE=0` torna toda cobertura incompleta, por premissa.


### 3.8 T3a.2: `blkcmp`, o que a GPU tinha contra o que o upload manda (implementado)

Escopo: o bloco "T3a.2" da seção 11 (revisões 5 e 6). **Implementado em `e574de8`, versão síncrona, aguardando o build do usuário; a revisão 6 manda sobre este texto onde divergiam, e a seção abaixo descreve o que foi implementado.** Não muda: o protocolo dos slots e dos lotes, a P0, a
cobertura da 3.7, nem o que o emulador faz com o guest, `gpu_modified_ranges`, `ClearDownload` e o rastreio de downloads.
Muda, **só para as faixas sorteadas**, o que acontece entre a sincronização do buffer e o upload: uma cópia diagnóstica
do que a GPU tem, com um `Finish`. Graus de certeza como no resto; números de linha são os de `47e4a8a`.

**Conferência do planejador (`8022a1d`, antes do revisor).** O T3a.1b foi reproduzido: três builds com 112 724
asserções em 53 casos; sem a linha do cursor, só o teste novo falha; os mutantes derrubam 6 (`coverage`) e 2 ou 3
(`open`, conforme o estresse) casos. Na 3.8 conferi: o `TickFrame` do cache de buffers roda sob o `buffer_cache.mutex`
(`vk_rasterizer.cpp:1194-1197`), e o `OnCachedWrites` também (`:974`). `Finish` submete e espera
(`vk_scheduler.cpp:67-75`). O `UsageTracker` (`usage_tracker.h`) é conservador numa página; o único defeito real é o
deslocamento de 64 bits com tamanho < 64 B (`:92`, que o UBSan acusa), preexistente. Dois pontos para a revisão:
1. **`is_draw_thread` só é ligado com as estatísticas** (`vk_rasterizer.cpp:338-340`). A contagem por thread da
   3.8.6 precisa de marca própria do diagnóstico, para não depender de `EDEN_BACK_THREAD_STATS`.
2. **Na versão síncrona, a ordem cópia → upload é garantida pelo `Finish`**, antes de o upload ser gravado. Um upload
   reordenado vai para o contexto seguinte, depois de a cópia ter terminado. O `MarkUsage` e o descarte
   `reorder_unprotected` só são necessários na versão assíncrona. Na síncrona, eles só acrescentam perturbação
   (este upload perde a reordenação, e o reset do rastreador atrasa) e descartes desnecessários. Proposta: tirá-los da
   versão 1 e deixá-los escritos como requisito da assíncrona.


**Conferência do planejador (`99328ab`, antes do revisor).** Os três builds foram reproduzidos (115 995 asserções, 61
casos); mutantes: `coverage` 6, `open` 2. O diff dos arquivos que não compilam na nuvem foi lido:
- `BlkcmpCapture` (`buffer_cache.h`): é a primeira coisa de `InlineMemoryImplementation`; busca o buffer só por
  `page_table` + `IsInBounds`; faz a cópia com `runtime.CopyBuffer(staging, buffer, copies, true)`, como
  `DownloadBufferMemory`, mas sem `MarkUsage`; depois o `Finish` e a comparação.
- O pedido é `static thread_local` (`protocol.h:1123`) e só é tocado sob o `buffer_cache.mutex`.
- O balde usa microfichas (taxa × µs) e limita na rajada.

**Perturbação a declarar (verificado):** toda submissão zera o contador de oclusão. `SubmitExecution` chama
`EndPendingOperations` (`vk_scheduler.cpp:384`), que faz `CounterReset(ZPassPixelCount64)` (`:448`). Cada `Finish` do
`blkcmp` acrescenta uma submissão no meio do frame e, com ela, um zeramento: as queries de oclusão do jogo podem mudar
numa rodada com `blkcmp`. Não afeta a comparação, mas a rodada não serve para julgar a imagem. É o defeito que o
Lichii do Frizti corrigiu pausando em vez de zerar (zortos293#121); está em `docs/gpu-contract-inventory.md` 7.5.

**Revisão 7 (sobre `fa7c63d`): diff aprovado para a compilação completa e a rodada diagnóstica.** Interpretação
obrigatória do resultado:
- **O que a comparação mede:** a comparação local continua válida. Ela compara os bytes capturados antes do upload com
  o payload preservado, e o reset das queries não escreve nesses bytes.
- **O que ela não mostra:** as queries podem mudar decisões do jogo e, com elas, os draws, os produtores de dados e as
  faixas elegíveis seguintes. **Uma diferença encontrada mostra uma substituição divergente na execução instrumentada.
  Ela não mostra que a mesma ocorrência, com a mesma frequência ou no mesmo caminho, aconteceria sem o `blkcmp`.** Uma
  diferença relevante tem de ser confirmada depois, com o contrato das queries corrigido (inventário 7.5) ou por uma
  reprodução dirigida sem essa interferência.
- **Ausência de diferença** continua não descartando o B.
- **Marca de thread:** a marca `Front` fica na thread. Se o split parar no meio da sessão (`gpu_split.cpp:82-86`, mudança
  de precisão), ela fica velha. Isso afeta só a classificação no relatório, e a marca não autoriza acesso ao scheduler.
#### 3.8.1 Do `NoteApply` até `InlineMemoryImplementation` (implementado)

Todos na **mesma thread** e dentro do mesmo `buffer_cache.mutex`: `RasterizerVulkan::OnCachedWrites` toma o mutex
(`vk_rasterizer.cpp:974`) e chama `CachedWriteMemory(addr, size)` por faixa (`:976-980`); o primeiro comando de
`CachedWriteMemory` é o `NoteApplyHook`, e o `InlineMemoryImplementation` vem dele.

1. **Decisão (`AccountBlock`, ramo "afirmada", `protocol.h`; `BlkcmpOffer`).** O laço do `at_risk` guarda também a lista das
   partes `(início, comprimento)`, até `kBlkcmpParts = 16`. Se `blkcmp_on` e `at_risk > 0`: conta `blkcmp_eligible` e sorteia
   (3.8.5). Sorteada: mais de 16 partes → `blkcmp_discard_ranges` (descartada, não truncada); `at_risk` acima do teto →
   `blkcmp_discard_size`; senão grava o `BlkcmpRequest` num `thread_local` `{addr, size, mask, lots, parts[], count, total}`.
2. **Escopo (`CachedWriteMemory`, `BlkcmpScope` logo depois do `NoteApplyHook`).** O destrutor, se o pedido ainda existe,
   conta `blkcmp_discard_no_upload`. Cobre **todos** os retornos antes do upload: `!is_dirty`, `!IsRegionGpuModified` →
   `WriteMemory` e qualquer outro. Desligado: a flag e nada mais.
3. **Captura (`InlineMemoryImplementation`, a PRIMEIRA linha, antes de `ClearDownload`).** `BlkcmpPending(dest_address,
   copy_size)` devolve o pedido só se `(addr, size)` for o da faixa sorteada (outro `(addr, size)`: `blkcmp_discard_stale`);
   o outro chamador do método (a escrita inline do motor) não tem pedido. Se há pedido, `BlkcmpCapture` roda **antes** de
   qualquer outra coisa do método.

**Por que a captura é a primeira linha (revisão 6).** O `InlineMemoryImplementation` faz `ClearDownload`, subtrai a faixa
de `gpu_modified_ranges`, `FindBuffer` e `SynchronizeBuffer`, e este sobe as faixas da CPU fora de `gpu_modified_ranges`,
então pode sobrescrever os bytes `at_risk` antes de uma cópia diagnóstica tardia. A frase "a posição não importa" da versão
anterior desta seção estava **errada** (guardar endereços não guarda valores). O que o upload desta aplicação sobrescreve
(`SynchronizeBuffer`, o próprio upload) é gravado **depois** do `Finish` da captura; o que veio antes já está na fila e é de um
produtor anterior.

#### 3.8.2 Os passos do lado do Vulkan (síncrono; `BufferCache::BlkcmpCapture`, `buffer_cache.h`)

| # | Passo | Chamada |
| --- | --- | --- |
| 1 | `USE_MEMORY_MAPS` falso (a constante do backend): `blkcmp_discard_no_memory_maps` e segue o código de hoje | `if constexpr` |
| 2 | **Buffer por consulta pura:** `page_table[dest_address >> CACHING_PAGEBITS]` e `IsInBounds(dest_address, copy_size)`. **Nunca `FindBuffer`**, que pode chamar `SynchronizeBufferWrites` e criar buffer (`buffer_cache.h:1745-1775`). Sem um buffer único que contenha a faixa: `blkcmp_discard_no_single_buffer` | `page_table`, `IsInBounds` |
| 3 | Ficha do balde (3.8.5). Sem ficha: `blkcmp_discard_budget` | `BlkcmpTakeToken` |
| 4 | Staging **próprio** de download, `DownloadStagingBuffer(total)`, `total` = a soma das partes `at_risk`, cada uma alinhada a 64 B (`BlkcmpLayout`) | `vk_buffer_cache.cpp:392` |
| 5 | Cópias `{src_offset = buffer.Offset(parte), dst_offset = staging.offset + desloc, size}` e `runtime.CopyBuffer(staging.buffer, buffer, copies, true)` (com barreira) | `vk_buffer_cache.h:142` |
| 6 | `runtime.Finish()`, cronometrado (`steady_clock`) | `vk_scheduler.cpp:67-75` |
| 7 | `BlkcmpCompare(mapped_span.data(), desloc, inlined_buffer, finish_us)` | 3.8.3 |
| 8 | O código de hoje de `InlineMemoryImplementation`, **sem nenhuma mudança** | `ClearDownload` em diante |

**Fora desta versão: `MarkUsage` e `reorder_unprotected`. A versão assíncrona PRECISA deles.** Na síncrona, a ordem cópia →
upload é garantida pelo `Finish`, que termina antes de o upload ser gravado; um upload reordenado (`CanReorderUpload`,
`vk_buffer_cache.cpp:448-458`) vai para o contexto seguinte, depois de a cópia ter terminado. Sem `Finish` (cópia diferida por
fence), a cópia diagnóstica ficaria no cmdbuf principal enquanto um upload reordenado executaria **antes** dela: a assíncrona
tem de marcar a região (`MarkUsage` do par exato `(buffer.Offset(dest_address), copy_size)` que o upload testa), conferir
`IsRegionUsed` (o `UsageTracker` não marca se o buffer passa do rastreador, e tem deslocamento indefinido para tamanho < 64 B:
`usage_tracker.h`) e descartar como `reorder_unprotected` se a conferência falhar. Isso **não** está implementado.

**Efeito sobre o resto:** a única mudança de estado do cache é a cópia (barreira, `Finish`, staging do pool). O guest
(`WriteBlockUnsafe` nunca é chamado), `gpu_modified_ranges`, `ClearDownload` e o rastreio de downloads e de leituras da CPU não
mudam, e `DownloadBufferMemory` (que escreve no guest e mexe no rastreio: `:2145-2191`) **não** é usado.

#### 3.8.3 A comparação (`Protocol::BlkcmpCompare`, `protocol.h`)

A referência é o **`inlined_buffer`** que o chamador já preencheu (`tmp_buffer`, `buffer_cache.h:159-162`): o diagnóstico
**não relê o guest**. Para cada parte `at_risk`: `antigo = staging[desloc]`, `novo = inlined_buffer[parte.addr - request.addr]`.
Conta bytes comparados (só os `at_risk`; o preenchimento do alinhamento não entra) e diferentes, a primeira diferença
(endereço e 8 bytes de cada lado, little endian), a thread, o tempo do `Finish`. A faixa conta `blkcmp_equal` ou
`blkcmp_different`.

**O que o resultado diz:** "bytes antigos da GPU diferentes do payload que os substituiu". Não prova corrupção visível (os
bytes da GPU podem ser lixo velho, ou a diferença pode ser uma escrita legítima concorrente do jogo entre o aviso e o upload),
e não vale para as faixas não sorteadas, incompletas ou ambíguas. As afirmadas **não são uma amostra aleatória** de todas as
aplicações (8.5). O resumo e o log dizem isso.

#### 3.8.4 O que fica fora

Versão assíncrona (por fence) e o que ela exige (3.8.2), T3a.3 (juntar registros por bloco), taxas separadas por parte, ligar
por ID de jogo, o ramo Android do upload (`buffer_cache.h:2008-2028`). Backend sem `USE_MEMORY_MAPS`: descartado e contado.

#### 3.8.5 Sorteio, balde, teto e perturbação (implementado)

- **Chave:** `EDEN_LIST_DIAG=...,blkcmp`. Exige `blk` (sem ele a linha "ligado" diz `blkcmp IGNORADO: exige blk`). **Não entra
  em `1`**: perturba.
- **Sorteio com semente própria.** `EDEN_LIST_DIAG_BLKCMP_SEED`; padrão `SplitMix64(sample_seed ^ 0x626c6b636d70)`: uma
  semente de sessão reproduz tudo e a constante tira a correlação com o predicado das páginas. Probabilidade `1/M`
  (`EDEN_LIST_DIAG_BLKCMP_ODDS`, **M = 4**): a faixa elegível de ordem `k` é sorteada sse `SplitMix64(seed ^ k) % M == 0`. As
  aplicações são serializadas (P0 e `buffer_cache.mutex`), então `k` é uma sequência única e a mesma semente dá o mesmo sorteio.
- **Balde:** taxa sustentada de `EDEN_LIST_DIAG_BLKCMP_BUDGET` (**5/s**) e rajada de `EDEN_LIST_DIAG_BLKCMP_BURST`
  (**5**); começa cheio, recarrega continuamente pelo relógio de microssegundos do diagnóstico, não passa da rajada. A ficha é
  tomada no **consumo** (passo 3 da 3.8.2), não no sorteio: descartar por tamanho, mapas, buffer ou "sem upload" não gasta ficha.
- **Teto:** `EDEN_LIST_DIAG_BLKCMP_MAXBYTES` (**4096**) de bytes `at_risk` por faixa.
- Valor inválido (não numérico, zero, negativo, fora do limite) volta ao padrão e a linha "ligado" lista quais.
- **Perturbação (declarada):** por comparação, um `Finish` sob o `buffer_cache.mutex`, mais a cópia. O `Finish` espera todo o
  trabalho já submetido, então o custo cresce com a fila da GPU; o teto é 5 esperas por segundo em regime. Medido só pelo
  `blkcmp_finish_us_*`. A rodada de `blkcmp` serve para achar ocorrência, **não** para medir draws/s.

#### 3.8.6 A thread (implementado)

A que grava o upload: a de trás (com o split), ou a da frente depois de um `DrainBack` completo (`puller.cpp:179-180`). **Marca
`thread_local` própria do diagnóstico** (`list_diag/thread_mark.h`, independente de `BackStats` e de `is_draw_thread`, que só é
ligado com as estatísticas): `GpuSplit::BackLoop` marca **Back** no começo da thread; `DrainBack` (`puller.cpp`) marca **Front**
na thread que drenou, e só com o split ativo. Sem marca (por exemplo sem o split, onde não há `DrainBack`), a comparação conta
`blkcmp_compared_other` (other/unknown), **nunca** como frente. Os três são contados separadamente.

#### 3.8.7 Contadores, log e resumo (implementado)

`blkcmp_eligible`, `_drawn`, `_discard_ranges`, `_discard_size`, `_discard_no_upload`, `_discard_no_memory_maps`,
`_discard_no_single_buffer`, `_discard_budget`, `_discard_stale`, `_compared` (`_back`, `_front`, `_other`), `_equal`,
`_different`, `_bytes_compared`, `_bytes_different`, `_finish_n`, `_finish_us_sum`, `_finish_us_max`. Identidades no resumo
final: `eligible >= drawn`; `drawn = compared + todos os descartes`; `compared = equal + different`.

**Log** (`[ListDiag][blkcmp] different:`, as 32 primeiras e depois 1 a cada 64, dentro dos 512 do diagnóstico): `range=[0x..,+N)
cores=0x.. lot=.. parts=K bytes=B diff_bytes=D first=0x.. gpu_old=0x.. payload=0x.. thread=back|front|other/unknown
finish=..us`. **Linha "ligado":** sorteio, semente, balde, teto e "um Finish por comparação, sob o buffer_cache.mutex".
**Resumo:** "blkcmp: bytes antigos da GPU diferentes do payload que os substituiu: D de B bytes comparados (F de C faixas);
sorteadas S de E elegíveis (as afirmadas com at_risk>0 ... NAO sao amostra aleatoria de todas as aplicacoes); descartadas ...;
Finish n=.. soma=.. max=.. Isso nao prova corrupcao visivel nem exclui uma escrita legitima concorrente."

#### 3.8.8 Testes

**No harness (`tools/list-diag-harness/`, código real de `list_diag/*.h`), casos `blkcmp [1]` a `[8]`:** o sorteio (reproduzível,
semente própria, é a fórmula, 1/M a menos de 4σ, M = 1 sorteia tudo); o balde (rajada, taxa sustentada, recarga fracionada, teto
da rajada depois de ociosidade longa, descarte por orçamento); o teto de bytes e o de partes; o mapeamento dos bytes `at_risk`
(partes, deslocamentos alinhados a 64 B); a comparação com um **staging falso** (iguais, uma diferença na primeira e na última
posição, várias partes, preenchimento do alinhamento ignorado, a primeira diferença no log); cada descarte por motivo e a
identidade `drawn = compared + descartes`; o texto do resumo; nada sem `blk` ou com `blkcmp` desligado, e os mesmos contadores
de `blk` e de avisos com e sem `blkcmp`; a configuração e a marca de thread (sem marca → other/unknown, nunca frente; uma thread
não vê o pedido de outra).

**O que o harness NÃO cobre, e só se confere por leitura e no build do usuário:** `BufferCache::BlkcmpCapture` e a captura na primeira
linha de `InlineMemoryImplementation` (o harness nunca instancia o `BufferCache`); a consulta `page_table`/`IsInBounds`; a cópia
no Vulkan (`DownloadStagingBuffer`, `CopyBuffer` com barreira, `Finish`, a leitura do `mapped_span`); a ordem real dos comandos; o
custo do `Finish`; as marcas de thread em `gpu_split.cpp` e `puller.cpp`. A confirmação é o log do usuário
(`blkcmp_discard_no_single_buffer` e `_stale` baixos, `blkcmp_finish_us_max` coerente, `compared_back`/`front`/`other` coerentes
com o split). **O teste de sanidade "100% de diferenças com `gpu_old` zerado" foi retirado (revisão 6).**

#### 3.8.9 T3a.2b: `blkcmp` assíncrono (desenho, sem código)

**Objetivo e o que continua sendo.** Confirmar as ocorrências nos jogos **sem `Finish` extra e sem submissões a mais**, para o
resultado do `blkcmp` valer também para a execução normal de GPU. **Continua sendo execução instrumentada**: há uma cópia a
mais, barreiras a mais e a reordenação do upload fica restrita nas faixas observadas (`MarkUsage`). O resumo diz isso. A versão
síncrona (3.8.1 a 3.8.8) continua como modo padrão até a assíncrona ser verificada pelo usuário; a escolha é uma variável
(`EDEN_LIST_DIAG_BLKCMP_MODE=sync|async`). **Nada disto está implementado. A IMPLEMENTAÇÃO espera o T4.1** (o `UsageTracker` sem UB
e conservador; ponto 2): nada de contornar o rastreador com outro mecanismo (por exemplo, desligar a reordenação global).

**1. Captura na posição de hoje.** A primeira linha de `InlineMemoryImplementation` (`buffer_cache.h:2114-2118`), antes de
`ClearDownload`, nunca depois de `SynchronizeBuffer` (3.8.1: o `SynchronizeBuffer` sobe faixas da CPU, `:2002-2014`). Buffer por
consulta (`page_table`, `IsInBounds`), sem `FindBuffer`. Tudo o que muda é **depois da captura**: em vez de `Finish`, a cópia fica
pendente (ponto 4).

**2. Proteção contra TODAS as sobrescritas relevantes, não só o upload final.** O que pode gravar nos bytes lidos entre o
registro da cópia e a sua execução, e como cada um fica atrás da cópia:

| Sobrescrita possível | Como é ordenada depois da cópia |
| --- | --- |
| O upload final (`InlineMemoryImplementation`, `:2123-2133`) | `CanReorderUpload` (`vk_buffer_cache.cpp:448-458`) devolve `false` se `IsRegionUsed(dst_offset, size)` (`:455`); o upload vai pelo `Record` do cmdbuf principal (`:487-499`), depois da cópia, e não pelo `upload_cmdbuf`, que executa antes (`:480-486`) |
| Os uploads do `SynchronizeBuffer` (`MappedUploadMemory`, `:2093-2094`, `CanReorderUpload` por cópia) | o mesmo teste, cópia a cópia, contra as faixas marcadas |
| Outras escritas em ordem de comando (`CopyBuffer` do cache, `ClearBuffer`, DMA) | gravadas no cmdbuf principal **depois** da cópia, em ordem de gravação; a ordem entre elas é a das barreiras (ponto 3) |
| Substituição do buffer (`JoinOverlap`, `buffer_cache.h:1891-1912`, `DeleteBuffer`, `:2254`) | a cópia lê o `VkBuffer` antigo; o novo recebe a cópia do antigo **depois** dela, em ordem. **Não há prova de que o anel de destruição cubra a cópia** (revisão de `825c5b3`, item 2): `DelayedDestructionRing::Tick()` (`delayed_destruction_ring.h:20-23`) avança um índice e destrói o vetor daquela posição **sem consultar fence nem tick da GPU**; o cache o avança uma vez por `TickFrame` (`buffer_cache.h:118`), e `TICKS_TO_DESTROY` é 6 ou 8 frames (`buffer_cache_base.h:525-527`). Frames não são ticks da GPU: com a GPU atrasada mais que isso, o `VkBuffer` pode ser destruído antes de a cópia executar. O staging diferido protege o **destino**, não a **fonte**. **Decisão:** reter a fonte até o maior tick, marcando as entradas, e devolvê-la ao anel (abaixo, "Retenção da fonte") |

**`MarkUsage` da região lida.** O mecanismo que alimenta `CanReorderUpload` é `Buffer::MarkUsage` (`vk_buffer_cache.cpp:125-128`,
`usage_tracker.h:40`). A marca cobre **os bytes lidos (as partes `at_risk`)** e, para o upload final, o par exato
`(buffer.Offset(dest_address), copy_size)` que ele testa (3.8.2, versão anterior desta seção). **Depende do T4.1**: o rastreador
tem de ser conservador, sem UB: nenhum bloco tocado deixa de ser marcado nem reconhecido, **inclusive** faixas que cruzam 64 B,
que cruzam a página do rastreador, que terminam exatamente no fim e de tamanho zero; hoje ele sai sem marcar quando o buffer passa
do rastreador (`usage_tracker.h:51-53`, `:71-73`) e tem deslocamento indefinido para tamanho < 64 B (`:92-94`). A conferência
`IsRegionUsed` logo depois do `MarkUsage` continua, como descarte contado (`blkcmp_discard_reorder_unprotected`): se o rastreador
não reconhece a marca, **não se compara**.

**3. Dependências Vulkan nos dois sentidos.** `MarkUsage` ordena só a reordenação do upload; **não substitui barreiras**.
- *As escritas anteriores ficam visíveis à cópia:* `runtime.CopyBuffer(staging, buffer, copies, true)` grava a barreira anterior
  (`READ_BARRIER`: escrita de memória → leitura/escrita de transferência, `vk_buffer_cache.cpp:466-471`, `:489-491`), a mesma
  que o `DownloadBufferMemory` usa (`:2176-2189`).
- *A cópia termina de ler antes das sobrescritas posteriores:* a barreira posterior (`WRITE_BARRIER`, de `TRANSFER` para
  gráficos/compute, `:492-499`) dá a dependência de execução. Uma leitura seguida de escrita (WAR) precisa só de dependência de
  execução, e ela existe no fim do `Record` da cópia; os `CopyBuffer` posteriores têm a barreira anterior própria.
- *A escrita no staging fica visível à CPU: invalidação (revisão de `825c5b3`, item 1).* A alternativa "a espera do tick a torna
  visível" **sai**: a conclusão da GPU não substitui a invalidação de memória não coerente (a memória de download só *prefere*
  `HOST_CACHED | HOST_COHERENT`, `vulkan_memory_allocator.cpp:96-99`, e a alocação usa `requiredFlags = 0`, `:289-293`; inventário 7.7).
  Depois de `IsFree(tick)` e **antes** da comparação, o `blkcmp` invalida o staging por `StagingBufferPool::Invalidate(ref)` (3.8.10).

**Retenção da fonte (revisões de `825c5b3` e de `29ea216`).** O ajuste anterior (segurar o buffer até o tick **da cópia**) destruía cedo
demais: o buffer pode ter **outros usos depois da cópia** (cópia no tick `T`, outro uso em `T+1`, `DeleteBuffer`, `T` termina com `T+1` ainda
executando), e pode haver **várias capturas pendentes da mesma fonte**. A regra é:
- **Todas as capturas da fonte:** `DeleteBuffer` (`buffer_cache.h:2254`; o `delayed_destruction_ring.Push` está em `:2312`) percorre a fila do
  `blkcmp` e considera **todas** as entradas pendentes cujo `buffer_id` é o deste buffer e que ainda não foram marcadas `source_released`.
- **O maior tick:** o buffer é retido até o **maior** `tick` entre essas entradas, e não até o da primeira (uma cópia mais nova pode estar
  num tick maior; o uso posterior à cópia mais nova **não** é coberto por nenhum dos ticks das cópias, e por isso a retenção usa
  `max(tick das cópias, CurrentTick() no momento do DeleteBuffer)`: o tick de agora cobre tudo o que já foi gravado até a remoção).
- **Contra o reuso do `buffer_id`:** todas essas entradas são marcadas `source_released` na mesma passada (o `SlotVector` pode dar o mesmo
  `buffer_id` a um buffer novo, e uma entrada marcada nunca mais retém o buffer novo; uma entrada **nova**, capturada depois, é sobre o
  buffer novo e não sobre o removido).
- **O objeto vai para a lista de retenção** `BufferCache::blkcmp_held_` (`{Buffer, tick}`), **em vez de** ir para o anel. A lista é
  esvaziada nos mesmos pontos de conclusão, quando `IsFree(tick)`.
- **No fim da retenção, devolve ao mecanismo normal de destruição adiada:** quando `IsFree(tick)`, o objeto é **empurrado para o
  `delayed_destruction_ring`** (`Push`, o mesmo de `:2312`), **nunca destruído direto**. A alternativa seria provar que todos os usos anteriores
  à remoção terminaram, o que exige uma garantia mais ampla que esta; devolver ao anel mantém a garantia que o resto do cache já
  supõe depois da retenção.
- O caminho normal do `DeleteBuffer` (sem entrada pendente, ou diagnóstico desligado, ou modo síncrono) **não muda**. `JoinOverlap`
  (`:1911`) e o coletor de lixo (`:79`) chegam por `DeleteBuffer`. A retenção não força submissão.

**4. Staging reservado até a comparação na CPU terminar, com o mecanismo diferido.** `runtime.DownloadStagingBuffer(total, true)`
(`vk_buffer_cache.cpp:392`, `vk_staging_buffer_pool.cpp:103-108`): o `TryGetReservedBuffer` marca a entrada com `tick = max` e
`deferred = true` (`:200+`), e nenhuma outra requisição a reaproveita (`is_free` exige `!entry.deferred`, `:187-189`) até o
`FreeDeferred` (`:110-120`), que grava `tick = CurrentTick()` e `deferred = false`. **Quem libera, e uma vez só:** a entrada da fila
do `blkcmp` tem um estado (`em voo` → `comparando` → `liberada`) e é a **única dona** do `StagingBufferRef`; a liberação
(`runtime.FreeDeferredStagingBuffer`) acontece no mesmo passo que tira a entrada da fila, sob o `buffer_cache.mutex`, que
serializa todos os pontos de conclusão (ponto 7). O `FreeDeferred` tem `ASSERT(it->deferred)` (`:117`): uma segunda liberação
acusa. A fila é **separada** do `async_buffers_death_ring` do download normal (`buffer_cache.h:119-123`, `:739`), e o `index`
do staging é único, então os dois nunca liberam o mesmo.

**5. Payload próprio e imutável.** Na captura, a entrada **copia** os bytes comparados do `inlined_buffer` (só as partes
`at_risk`, ≤ 4096 B no total, num vetor da própria entrada), mais o mapeamento das partes `(addr, size, desl_no_staging)` e os
metadados `{addr, size, mask, lots, papel da thread, tick, instante do sorteio}`. Nada guarda ponteiro para o `tmp_buffer` (que
o próximo `CachedWriteMemory` reaproveita, `buffer_cache.h:159-162`), para o `BlkcmpRequest` `thread_local` (que o escopo apaga) nem
para um `Buffer` (que `DeleteBuffer` pode remover): a comparação só precisa do staging, que a entrada possui, e do payload copiado.

**6. Conclusão ligada à submissão que contém a cópia.** Depois de gravar a cópia, a entrada guarda `tick = runtime.CurrentTick()`
(`vk_buffer_cache.cpp:431-433`), lido **depois** do `Record` (a cópia está no contexto desse tick; nenhuma submissão acontece
dentro de `Record`: ela é explícita, `vk_scheduler.cpp:67-75`; **a conferir na implementação**, com uma asserção de que o tick não
mudou entre antes e depois da gravação). **Por que a leitura depois do `Record` basta** (revisão de `825c5b3`, item 4): os ticks só
crescem; a cópia foi gravada no contexto de tick `T0`, e `T1 = CurrentTick()` lido depois é `>= T0`, então esperar `IsFree(T1)` é
conservador (no pior caso uma submissão alheia entre a gravação e a leitura atrasa a conclusão). O que seria **inseguro** é ler antes
da gravação. `RecordWithUploadBuffer` só grava no chunk ou chama `DispatchWork()` (`vk_scheduler.h:107-113`); quem avança o tick é o
`SubmitExecution`, explícito (`vk_scheduler.cpp:67-75`). **A exclusão entre os produtores do scheduler é requisito próprio, não só do argumento do tick**
(revisão de `29ea216`): o `Record` mexe em estado compartilhado do scheduler (o chunk, o contexto), e um tick que só cresce não torna seguras
chamadas concorrentes. A captura grava a cópia sob o `buffer_cache.mutex`, na thread que aplica, e só é correta se **nenhuma outra
thread grava no scheduler ao mesmo tempo**: é a convenção que o resto do código do rasterizador já segue (a trás, ou a frente depois de
`DrainBack`; 3.3). A implementação assíncrona a declara como precondição, como a P0, e **não** a faz valer, nem a detecta por completo. A comparação
"tick antes == tick depois da gravação" detecta **só mudança de contexto durante a captura** (uma submissão no meio); **não**
detecta toda violação da exclusão, porque dois produtores podem gravar ao mesmo tempo sem mudar o tick. A exclusão continua
sendo precondição, sem mecanismo novo. A cópia terminou quando `runtime.IsFree(tick)` (`KnownGpuTick() >= tick`,
`vk_master_semaphore.h:42`), a mesma regra pela qual o pool reaproveita um staging (`vk_staging_buffer_pool.cpp:187-189`).
**Nunca** o syncpoint do guest nem um callback de fence do jogo (em Fast eles correm cedo), e **nunca** forçar uma submissão:
`Finish`, `Flush` e `Wait` ficam fora. Uma entrada cujo tick não passou fica pendente, sem custo.

**7. Fila limitada e encerramento.**
- *Limites:* `EDEN_LIST_DIAG_BLKCMP_INFLIGHT` pedidos em voo (padrão **8**) e **64 KiB** de staging em voo (a soma de `total`). No limite, a
  observação **nova** é descartada (`blkcmp_discard_queue_full`) **sem bloquear** e sem sair do ramo normal do upload.
- *Onde a comparação roda:* na CPU, sob o `buffer_cache.mutex`, em **dois pontos de conclusão**: (a) no começo de
  `BufferCache::BlkcmpCapture` (a thread que aplica, a de trás ou a da frente depois de `DrainBack`), no máximo 4 entradas por
  chamada; (b) em `BufferCache::TickFrame` (`buffer_cache.h:85-123`, sob o mutex em `vk_rasterizer.cpp:1194-1197`), todas as
  prontas. Em ambos: `IsFree(tick)`, comparar o staging com o payload copiado (3.8.3), contar, **liberar o staging**, tirar da fila.
  O `papel da thread` contado é o da **captura**, guardado na entrada, não o de quem conclui. **Não** se usa o `PopAsyncBuffers`
  (`buffer_cache.h:728-736`) para processar a comparação: ele é o caminho do download publicado (problema A) e acoplar um ao
  outro misturaria os dois diagnósticos.
- *Encerramento (revisão de `825c5b3`, itens 2 e 3).* Ordem: (1) no resumo (`NotifyShutdown`, `gpu.cpp:255`), **primeiro** processa toda
  entrada cujo tick já é livre (invalida, compara, libera), e **depois** classifica o que sobra na fila como **pendente** (tick ainda
  não livre). "Pendente" nunca inclui uma entrada concluída e não processada. A identidade impressa ali sai marcada
  **fotografia intermediária** (pendentes ainda podem completar). (2) Na destruição do `BufferCache`, o corpo de
  `RasterizerVulkan::~RasterizerVulkan` já executou `scheduler.WaitWorker()` e `scheduler.Finish()` (`vk_rasterizer.cpp:249-252`) e o
  `RendererVulkan::~RendererVulkan` faz `WaitIdle` (`renderer_vulkan.cpp:201-204`); os membros são destruídos em ordem inversa da
  declaração, então `buffer_cache` (`vk_rasterizer.h:230`) vai antes de `staging_pool` (`:219`), os dois **depois** do `Finish`. É esse caminho,
  e não o destrutor do pool sozinho, que garante a GPU ociosa. Ali: o que tem tick livre é comparado e liberado; o que sobra (tick
  que não ficou livre, por exemplo dispositivo perdido) conta `blkcmp_cancelled` e **não é liberado** nem comparado, e as
  retenções da fonte seguem o mesmo critério. A **identidade final** é a impressa por essa destruição (`blkcmp final: drawn = compared
  + descartes + pendentes + cancelados`), depois de classificar os três.

**Contadores novos:** `blkcmp_pending` (medidor: entradas em voo agora, e `blkcmp_pending_max`), `blkcmp_cancelled`,
`blkcmp_discard_queue_full`, `blkcmp_discard_reorder_unprotected`, `blkcmp_latency_us_sum/_max/_n` (da captura à conclusão). A
identidade passa a ser `drawn = compared + descartes + pendentes + cancelados`, com o resíduo, só no resumo final. O modo vem na
linha "ligado" e no resumo (`blkcmp=async inflight=.. bytes=..`).

**Perturbação declarada (hipótese, a medir):** uma cópia de até 4 KiB, duas barreiras, a perda da reordenação **das faixas
marcadas**, o adiamento do reset do rastreador desse buffer até o `TickFrame` seguinte, e a comparação na CPU depois. Sem `Finish`,
o custo por comparação não cresce com a fila da GPU, então o orçamento padrão pode subir: **20/s sustentado, rajada 20 é um orçamento
experimental, não uma conclusão** (revisão de `825c5b3`, item 5): tirar o `Finish` remove aquela espera, **não toda a interferência**; continuam as
barreiras, as restrições de reordenação e a pressão na fila. A confirmar pela medição de `blkcmp_latency_us_*`, do `other busy` e do
`blkcmp_pending_max`.

**Testes.**
*No harness* (a máquina de estados é código de `list_diag/`, sem Vulkan, com um **relógio de ticks falso** e um **pool falso que
conta as liberações**): a entrada guarda o payload por cópia (mudar o buffer de origem depois da captura não altera a
comparação); só conclui quando o tick é livre (antes: pendente); conclusão fora de ordem de ticks; liberação **exatamente uma vez**
por entrada (a segunda tentativa não libera e é acusada); limite de pedidos e de bytes (`queue_full`, sem bloquear, a entrada nova
descartada); o papel da thread é o da captura; encerramento: comparados, pendentes e cancelados separados e a identidade; nenhum
`Finish`, `Flush` ou `Wait` chamado pelo caminho (o pool falso conta); o orçamento e o sorteio como na versão síncrona.
*Só por leitura e no build do usuário:* que o `MarkUsage` impede de fato a reordenação (precisa do T4.1); as barreiras e a
visibilidade ao host (ponto 3); que o tick lido depois do `Record` é o da cópia (ponto 6); o `delayed_destruction_ring` (ponto 2);
a liberação do staging deferido no `BufferCache` real; o custo. A confirmação é o log: `blkcmp_discard_reorder_unprotected` perto de
zero, `blkcmp_pending_max` abaixo do limite, `cancelled` zero em encerramento limpo, e as diferenças da versão síncrona
reaparecendo (ou não) com o mesmo padrão.


**Revisão de `825c5b3` (pelo revisor, conferida pelo planejador): o que falta antes de implementar o assíncrono. Os cinco pontos estão
incorporados acima (3.8.9, pontos 2, 3, 6 e 7, "Retenção da fonte" e o orçamento) e a invalidação está desenhada na 3.8.10.**
1. **Visibilidade do staging:** a memória de download não tem coerência garantida (`vulkan_memory_allocator.cpp:96-99`,
   `:289-293`; inventário 7.7). Sai a alternativa "a espera do tick a torna visível": a conclusão da GPU não substitui a
   invalidação de memória não coerente. O pool passa a dar acesso a `vk::Buffer::Invalidate()` (`vulkan_wrapper.cpp:534-538`)
   para o staging, e ela é chamada depois de `IsFree(tick)` e antes da comparação. O `StagingBufferRef` hoje só entrega o
   mapeamento. **O `blkcmp` síncrono também precisa disso**, depois do `Finish`, e deve registrar se a memória era
   coerente: o resultado da 8.6 supõe coerência.
2. **Vida do buffer fonte:** `DelayedDestructionRing::Tick()` (`delayed_destruction_ring.h:20-23`) avança e destrói uma
   posição sem consultar fence nem tick da GPU, e o cache de buffers o avança no `TickFrame`. O staging diferido protege o
   destino, não a fonte. Antes da implementação, documentar qual espera garante que a cópia terminou antes de o buffer
   fonte ser destruído. Se não houver, segurar o recurso até o tick da cópia. O mesmo para a afirmação de que o pool só é
   destruído com a GPU ociosa: citar o caminho de encerramento, porque o destrutor sozinho não prova isso.
3. **Encerramento:** "pendente" inclui toda entrada que ainda está na fila, mesmo com o tick concluído e sem comparação
   processada. A identidade final sai **depois** de classificar comparados, pendentes e cancelados, ou aparece marcada como
   fotografia intermediária.
4. **Tick depois do `Record`:** procede (`RecordWithUploadBuffer` não avança o tick; `SubmitExecution` avança), e
   continua dependendo da exclusão entre os produtores do scheduler.
5. **20/s** é orçamento experimental. Tirar o `Finish` remove aquela espera, não toda a interferência: continuam as
   barreiras, as restrições de reordenação e a pressão na fila.
**Revisão de `29ea216` (pendências da versão assíncrona, antes do código dela). O texto acima (3.8.9, "Retenção da fonte" e a exclusão
entre os produtores) e a 3.8.10 já a incorporam:**
- **A retenção da fonte pode destruir cedo demais.** O buffer pode ter usos depois da cópia (cópia no tick T, outro uso em
  T+1, `DeleteBuffer`, T termina com T+1 ainda executando), e pode haver várias capturas pendentes da mesma fonte. Ajuste
  mínimo:
  - considerar todas as capturas da fonte e usar o maior tick;
  - marcar todas as entradas contra o reuso do `buffer_id`;
  - no fim da retenção, **devolver o objeto ao mecanismo normal de destruição adiada**, nunca destruí-lo direto.
  A alternativa seria provar que todos os usos anteriores à remoção terminaram, o que exige uma garantia mais ampla.
- **A exclusão entre os produtores do scheduler** não é só requisito do argumento do tick: o `Record` mexe em estado
  compartilhado do scheduler, e um tick que só cresce não torna seguras chamadas concorrentes.
- **`staging=coherent` numa rodada nova** vale para as alocações daquela rodada; não prova o tipo das alocações antigas. A
  evidência que conta é a medição nova, com a invalidação certa.

#### 3.8.10 Invalidação do staging, também no `blkcmp` síncrono (desenho, sem código)

**O problema (inventário 7.7).** O `blkcmp` lê o `mapped_span` do staging de download sem invalidar, e o resultado da rodada 4 (8.6) **supõe
que o staging é coerente**; o log não diz. É a menor mudança para o resultado deixar de supor: invalidar antes de comparar e **registrar** se a
memória era coerente. A correção geral dos downloads do cache de buffers e de texturas (7.7) é **item próprio, depois**; não está aqui.

**Como o pool expõe a operação.** `vk::Buffer::Invalidate()` (`vulkan_wrapper.cpp:534-538`) chama `vmaInvalidateAllocation` só se `!is_coherent`,
e `IsHostCoherent()` (`vulkan_wrapper.h:813-815`) diz o tipo. O `StagingBufferRef` (`vk_staging_buffer_pool.h:22-30`) só tem o `VkBuffer`
e o mapeamento, mas a `StagingBuffer` do pool tem o `vk::Buffer` (`:55-76`). Duas funções novas, no mesmo estilo de `FreeDeferred`
(`vk_staging_buffer_pool.cpp:110-120`, que acha a entrada por `usage`, `log2_level` e `index`):
- `bool StagingBufferPool::IsCoherent(const StagingBufferRef&)`: acha a entrada e devolve `buffer.IsHostCoherent()`;
- `void StagingBufferPool::Invalidate(const StagingBufferRef&)`: acha a entrada e chama `buffer.Invalidate()` (não faz nada se coerente).
`BufferCacheRuntime` as repassa (`InvalidateStaging`, `StagingIsCoherent`), e o `gl_buffer_cache.h` ganha versões vazias (`true`/nada), porque o
código genérico do cache as chama. O custo é uma busca linear por entrada, só quando o `blkcmp` compara (poucas vezes por segundo).

**Onde chamar.** Em `BufferCache::BlkcmpCapture` (3.8.2), passo 6→7: **depois** do `runtime.Finish()` e **antes** do `BlkcmpCompare`:
```
const bool coherent = runtime.StagingIsCoherent(download_staging);
if (!coherent) runtime.InvalidateStaging(download_staging);
BlkcmpCompare(mapped, offsets, payload, finish_us, coherent);
```
No assíncrono, o mesmo, **depois** de `IsFree(tick)` e antes da comparação (3.8.9, ponto 3). O `Invalidate()` cobre a alocação inteira
(`VK_WHOLE_SIZE`); o staging é pequeno (poucas centenas de bytes a 4 KiB), então não vale uma versão por faixa.

**Campo no log e no resumo.** `Protocol::BlkcmpCompare` ganha o parâmetro `bool staging_coherent`. Contadores novos:
`blkcmp_staging_coherent` e `blkcmp_staging_invalidated` (`compared = coherent + invalidated`, uma identidade a mais no resumo final). A
linha de diferença do log ganha `staging=coherent|invalidated`. O resumo diz: `staging: coerente em N de M comparações, invalidado em K`.
**`staging=coherent` numa rodada nova vale para as alocações daquela rodada** (revisão de `29ea216`); **não prova o tipo das alocações antigas**,
nem que a 8.6 foi lida com memória coerente. A evidência que conta é a medição nova, com a invalidação certa. Interpretação: **se K for zero**,
a memória daquela rodada era coerente e o resultado novo não depende disso; **se K for maior que zero**, as
diferenças da 8.6 podem incluir leitura de dados velhos do próprio staging (nesse caso o resultado se repete com a invalidação antes de se
tirar conclusão). Até a primeira rodada com o campo, a 8.6 continua dizendo que **supõe coerência**.

**Testes.** *No harness:* o parâmetro e os dois contadores (identidade `compared = coherent + invalidated`), o campo do log e o texto do resumo, com
uma chamada de comparação falsa que passa `true` e `false`. *Só por leitura e no build do usuário:* `StagingBufferPool::IsCoherent/Invalidate` e
a chamada real do Vulkan (o harness não tem pool nem `vk::Buffer`); que `Invalidate` cobre o `mapped_span` lido.

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
  encontrado e outro perdido no mesmo bloco e lote, a lista `exact` fica incompleta e `at_risk` é afirmado.
  (Sem nenhum registro, o trecho já sai ambíguo, `protocol.h:695`: a afirmação contrária que estava aqui era um
  erro meu, corrigido pelo codador na 3.7.3.) Os 70% ambíguos vêm de
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

### 8.5 Medições do T3a.1 (rodada 3, 2026-11-06)

**Rodadas (medido pelo usuário; configuração conferida na linha `ligado` de cada log).** O build é de `c700e86` ou
posterior: a linha `ligado` traz a amostra, e a linha `Dirty 2 KiB` não tem mais a chave. FE no lugar de sempre
(25 s depois de carregar o save, parado), `EDEN_BACK_THREAD_STATS=1` em todas: 3a e 3c desligados; 3b e 3d com
`pop,blk` e `N=8`; 3e com `pop` e `N=1`. Bananza: uma rodada `pop,blk` com `N=8`. Nenhuma violação de P0. Zero
cruzamentos de 2 KiB. **Todas as identidades de conservação fecham com resíduo 0** nos quatro resumos.

**Custo no FE (amostras com a trás ≥ 85%):**

| Rodada | Diag | draws/s | `other busy` | Lotes de escritas seguradas |
| --- | --- | --- | --- | --- |
| 3a | desligado | 79 610 (5) | 15,0% | ~397/s, 27 trechos cada |
| 3c | desligado | 78 846 (4) | 15,0% | ~405/s, 27 |
| 3b | `pop,blk`, N=8 | 77 691 (4) | 16,2% | ~524/s, 21 |
| 3d | `pop,blk`, N=8 | 78 735 (5) | 16,3% | ~520/s, 21 |
| 3e | `pop`, N=1 | 71 394 (5) | 20,9% | ~857/s, 13 |

- **`pop,blk` com N=8: −1,3%** (79 228 → 78 213). O ruído entre rodadas iguais foi de 1,0% e 1,3%, então o custo está
  na borda do ruído e bem dentro do orçamento de 5%. No T3a era −20,0%. Medido com as estatísticas ligadas nos dois
  braços: vale para essa configuração (7.4.1).
- **A perturbação dos lotes diminuiu, mas não sumiu:** 1,3 vez mais lotes com N=8 (contra 2,6 vezes no T3a). Com
  `pop` e N=1, −9,9% e 2,1 vezes mais lotes, com o `other busy` em 20,9%. A perturbação acompanha o custo por aviso:
  é compatível com a hipótese 2 da 3.7.8, mas ainda não a prova.
- **`InvalidateGPUCache` (contadores do D1, mesmo trecho):** desligado ~1 800/s com ~400/s com dados; N=8 ~1 780/s
  com ~520/s com dados; N=1 ~1 630/s com ~860/s com dados. O número de chamadas quase não muda; muda quantas
  encontram dados.

**Cobertura e magnitude (resumos finais):**

| Medida | 3b (N=8) | 3d (N=8) | 3e (`pop`, N=1) | Bananza (N=8) |
| --- | --- | --- | --- | --- |
| Avisos vistos / amostrados (fração de avisos) | 28,5 mi / 3,43 mi (0,120) | 29,1 mi / 3,45 mi (0,119) | 26,7 mi / todos | 21,2 mi / 2,45 mi (0,116) |
| Amostrados que estouraram o ledger | **45%** | **45%** | **85%** | **54%** |
| Faixas amostradas sobre bytes da GPU | 22 636 | 19 412 | — | 6 569 |
| … ambíguas / incompletas / **afirmadas** | 47% / 10% / **43%** | 50% / 12% / **38%** | — | 74% / 9% / **16%** |
| B: `at_risk` nas afirmadas / `not_seen_cpu_read` | 387 KB / 92% | 283 KB / 89% | — | 44 KB / 92% |
| `at_risk` que o T3a teria afirmado nas incompletas | 404 KB | 408 KB | — | 147 KB |
| A: pops sobre aviso / registros / bytes | 6 / 62 / 268, todos `equal` | 3 / 29 / 124, todos `equal` | **24 / 1 144 / 5 252, todos `equal`** | 4 / 103 / 544, 1 `different` |
| Trocas de thread no `InvalidateGPUCache` | 1 | 1 | 1 | 24 249 |

Leitura:
- **As afirmadas não são amostra aleatória** (revisão 5): rajadas, lotes sobrepostos e estouro decidem quem chega lá.
  Uma diferença encontrada nelas confirma uma ocorrência; nenhuma diferença não descarta o B fora delas; a proporção
  nelas não vale para todas as aplicações.
- **O B existe nas faixas afirmadas.** São faixas com cobertura completa demonstrada: 43% e 38% das faixas amostradas
  do FE que cobrem bytes da GPU. Nelas a aplicação sobe por cima de bytes da GPU que a CPU não escreveu, ~40 B por
  faixa afirmada em média, e ~90% desses bytes estão fora de `cpu_read_ranges`. **Continua sendo limite superior**:
  falta saber se os bytes do guest diferem dos da GPU, que é o que o T3a.2 (`blkcmp`) mede.
- **O T3a superestimava o B.** Nas faixas que o T3a.1 declara incompletas, o T3a teria afirmado mais `at_risk` (404 KB) do
  que existe nas afirmadas (387 KB). Era o buraco do overflow parcial que o revisor apontou (revisão 4).
- **O estouro continua alto.** Com N=8 são ~78 mil avisos amostrados/s e vida média de 7,5 ms: pela lei de Little,
  ~570 registros vivos em média, abaixo dos 1 024 slots por núcleo. Mesmo assim, 45% estouram. **Causa provável
  (revisão 5, verificado no código):** `Register` examina no máximo 8 slots a partir de `cursor_` e só avança o cursor
  quando registra (`ledger.h:178`, `:232`). Depois de um estouro, o aviso seguinte examina os **mesmos** 8 slots
  ocupados, mesmo com o anel quase vazio. Então o estouro não prova anel cheio. Rajadas são a outra hipótese. Como `floor_raises` é pequeno (1 860), o
  estouro se concentra em poucos lotes, e por isso só 10% das faixas saem incompletas.
- **~60 avisos por trecho final** (~650 mil avisos/s contra ~11 mil trechos/s) **não** prova repetição no mesmo bloco de
  64 B, porque um trecho tem vários blocos (revisão 5). Juntar registros por `(núcleo, lote, bloco)` (T3a.3) fica
  **condicional**: só se o estouro continuar depois do avanço do cursor e o resultado do `blkcmp` pedir mais cobertura.
- **A, com todas as páginas (3e):** 24 pops sobre 1 144 registros, todos `sample_equal`, com 85% de estouro. Continua
  sendo um piso com cobertura de ~15%.
- **Bananza:** ambíguo domina (74%), por sobreposição com outros lotes (`apply_overlap_ambiguous_lot` 9 265). As trocas
  de thread (24 mil) não violam a P0. Sem rodada desligada, não há custo.

### 8.6 Medições do T3a.1b e do T3a.2 (rodada 4, 2026-11-06)

**Rodadas (medido pelo usuário; uma de cada configuração, por escolha dele: repetição só para confirmar algo
inesperado).** O build é de `cb3c982`: a linha `ligado` traz o `blkcmp`. FE no lugar de sempre, com
`EDEN_BACK_THREAD_STATS=1`: 4a desligado, 4b `pop,blk`, 4c `pop,blk,blkcmp`, 4d `pop,blk,blkcmp` com
`EDEN_LIST_DIAG_BLKCMP_SEED=2`. Bananza: `pop,blk,blkcmp`. Sem violação de P0. Todas as identidades de conservação,
inclusive as do `blkcmp`, fecham com resíduo 0. Os `Unmapped Device ReadBlock` (213 por rodada no FE) e os 4
`assert false` de `memory_manager.cpp:296` no encerramento do 4d são preexistentes: os mesmos números aparecem nas rodadas
3 e no log `d` da rodada 1.

**Custo (amostras com a trás ≥ 85%):** 4a 78 759; 4b 76 709 (**−2,6%**, um par só; na rodada 3, dois pares davam −1,3%);
4c 77 058; 4d 77 186. O `blkcmp` não mostra custo próprio em draws/s nesta medida. O `Finish` dele soma 173 ms e 157 ms na
sessão (~0,4% do tempo), **com picos de 20 e 30 ms**: isso perturba o frame, mas some na média.

**T3a.1b (cursor):** os amostrados que estouram o ledger foram de 45% (rodada 3) para **43,8%** (4b). **A hipótese do cursor
parado não explica o estouro.** Sobra a rajada, ou mais registros vivos do que a média sugere (**hipótese**). Ambíguas,
incompletas e afirmadas ficam como antes: 47%, 14% e 40% em 4b.

**T3a.2 (`blkcmp`):**

| | 4c (FE) | 4d (FE, semente 2) | Bananza |
| --- | --- | --- | --- |
| Elegíveis (afirmadas com `at_risk > 0`) / sorteadas | 10 455 / 2 589 | 9 891 / 2 381 | 796 / 189 |
| Comparadas / descartadas pelo orçamento | 193 / 2 396 | 188 / 2 193 | 65 / 122 (mais 2 por número de partes) |
| **Faixas com bytes diferentes** | **6 de 193 (3,1%)** | **4 de 188 (2,1%)** | **4 de 65 (6,2%)** |
| **Bytes diferentes / comparados** | **89 / 7 820 (1,1%)** | **65 / 7 724 (0,8%)** | **56 / 3 060 (1,8%)** |
| Thread | trás 193 | trás 188 | trás 59, frente 6 |

**Bytes antigos da GPU diferentes do payload que os substituiu: há ocorrências nos dois jogos.** Dois padrões nas
linhas de log:
- **FE, valores próximos.** Todas as faixas diferentes caem em poucas páginas (`0x810bb000` a `0x810ec000`). As palavras
  parecem floats próximos: a GPU tinha 0,42512 e o upload trouxe 0,41862; −1,20754 e −1,20370; 14,59291 e 14,58794. É o
  formato de um dado que a GPU atualiza a cada frame, sobrescrito por uma cópia mais velha que está no guest
  (**hipótese**).
- **Zeros por cima de dados da GPU.** Duas faixas no FE e as quatro do Bananza: o payload é zero onde a GPU tinha valor
  (por exemplo, `0xb727c5ad` em duas faixas do Bananza, a 1 s de distância). É o formato de um guest que nunca recebeu o
  dado da GPU (**hipótese**; ~90% dos bytes em risco estão fora de `cpu_read_ranges`).

**Interpretação (revisão 7):**
- **O que isto demonstra:** substituições divergentes **na execução instrumentada**. As faixas são afirmadas, então
  todos os avisos daquele lote estão em `exact`, e os bytes comparados são os que a CPU não escreveu nele.
- **O que isto não demonstra:** que a mesma ocorrência, frequência ou trajetória aconteceria sem o `blkcmp`, porque o
  `Finish` dele zera queries de oclusão. Nem demonstra corrupção visível.
- **As taxas** (2 a 6% das faixas comparadas) valem só para as afirmadas sorteadas, e as afirmadas não são amostra
  aleatória.
- **Sobra uma limitação do T3a inteiro (3.5):** o aviso registrado não é escrita concluída.

### 8.7 Medições do T4.1 e do `blkcmp` com staging conferido (rodada 5, 2026-11-06)

**Rodadas (medido pelo usuário; uma de cada; o build `927077a` compilou no MSVC).** FE, `EDEN_BACK_THREAD_STATS=1`:
5a com o rastreador novo; 5b com `EDEN_USAGE_TRACKER_OLD=1`; 5c com `pop,blk,blkcmp` e o rastreador novo. A linha
`Upload reordering` diz o rastreador ativo em cada uma. Sem P0; todas as identidades fecham com resíduo 0.

**Método (corrigido):** as rodadas são alinhadas pelo **início da cena**, o primeiro relatório com mais de 40 mil draws/s.
Descarta-se esse relatório, que ainda é transição, e usa-se a média dos 5 seguintes. O usuário sempre para ~25 s depois
de o save carregar, então essa janela cobre o mesmo trecho em todas as rodadas, qualquer que seja a demora do emulador e
do menu. O filtro antigo ("trás ≥ 85% ocupada") descartava amostras válidas da 5a e não alinhava as rodadas.

**Custo do T4.1 (medido; uma repetição a pedido do planejador, 5a2):**

| Rodada | Rastreador | Cena começa | 5 amostras seguintes (draws/s) | Média |
| --- | --- | --- | --- | --- |
| 5a | novo | 101 s (o emulador e o menu demoraram) | 94 132, 85 048, 77 514, 73 462, 79 433 | 81 918 |
| 5a2 | novo | 37 s | 81 138, 74 879, 76 647, 75 670, 78 207 | 77 308 |
| 5b | antigo | 36 s | 81 016, 79 093, 80 571, 80 059, 80 771 | 80 302 |

Novo (duas rodadas) 79 613 contra antigo 80 302: **−0,9%**. As duas rodadas iguais diferem entre si em 5,8%, então o ruído
desta campanha é bem maior que o ~1% da rodada 3. **Conclusão: nenhum sinal de custo do T4.1**, dentro de um ruído grande.

**Reordenação (medido):** a fração de uploads que vai para o `upload_cmdbuf` no trecho estável foi:

| Rodada | Rastreador | Fração reordenada |
| --- | --- | --- |
| 5a | novo | 18-21% (~2 100/s) |
| 5c | novo, com diagnóstico | 15-17% (~1 700/s) |
| 5a2 | novo | **10-11%** (~1 150/s) |
| 5b | antigo | 9-10% (~1 100/s) |

**A fração varia de uma rodada para outra com o mesmo rastreador** (5a contra 5a2), então a diferença não pode ser
atribuída ao T4.1. A primeira versão desta seção dizia que o rastreador novo reordena mais; isso foi retirado. Não há sinal
de que ele reordene menos. Permissão e efeito batem em todas: toda cópia permitida vem do stream buffer.

**`blkcmp` (5c):**
- **staging coerente em 203 de 203 comparações**, invalidado em 0, nenhum `staging_nao_achado`. Isso vale para as
  alocações desta rodada (revisão de `29ea216`).
- Diferenças: **6 de 203 faixas** (91 de 7 732 bytes), de novo nas páginas `0x810bb000` a `0x8111f000`, com floats próximos
  (por exemplo `0xc0f8aa87` contra `0xc0f9142e`).
- O `Finish` teve máximo de 5,2 ms (na rodada 4, 20 a 30 ms).
- O resultado da 8.6 deixa de depender de supor coerência **nesta máquina**: com staging coerente medido, o mesmo padrão
  aparece.

**Quando as diferenças aparecem (medido, nas quatro rodadas com `blkcmp`):** todas antes de a cena estabilizar, durante a
carga do save. Pelos totais de 30 s:

| Rodada | Até o primeiro total (fim da carga) | Depois, na cena estável |
| --- | --- | --- |
| 4c (FE) | 125 comparadas, 6 diferentes (até 46 s) | 68 comparadas, 0 diferentes |
| 4d (FE) | 124 comparadas, 4 diferentes (até 45 s) | 64 comparadas, 0 diferentes |
| 5c (FE) | 59 comparadas, 6 diferentes (até 46 s) | 130 comparadas, 0 diferentes |
| Bananza (rodada 4) | 35 comparadas, 4 diferentes (até 33 s) | 30 comparadas, 0 diferentes |

São ~240 comparações com 20 diferenças na carga, contra ~290 comparações com nenhuma diferença na cena estável.
**Leitura (hipótese):**
- O B aparece quando a cena é montada: dados que a GPU produz uma vez (ou nos primeiros frames) são sobrescritos pela
  cópia velha do guest.
- No regime estável, as escritas seguradas não pegam esses dados.
- Como o resultado da carga pode persistir (um buffer montado errado continua errado), isso não diminui a importância
  do B, mas muda onde procurar o efeito: na montagem da cena, não no custo por frame.
- As faixas sorteadas não são amostra aleatória, e o orçamento limita as comparações por segundo. A falta de diferenças
  na cena estável não prova que elas não existam ali.

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
| 14 (revisão 4, sobre `dc4d0a9`) | (1) Com o diagnóstico ligado, um aviso de página não amostrada deve continuar em `CollectT<true>`: misturar com `CollectT<false>` no mesmo gerenciador quebra as premissas da prova, porque os avisos não amostrados também mexem no mesmo `current` e empurram dados para `back_buffer`. (2) Overflow parcial no mesmo bloco e lote deixa `exact` incompleta sem marcar ambíguo; pede teste e mecanismo conservador. (3) Amostra por página, sim; predicado pelo mesmo endereço em todos os hooks, independente do núcleo, com `N` e semente fixos e registrados; resultados observados em primeiro lugar, sem multiplicar tudo por `N`. (4) Recorte do pop por página só no diagnóstico, com o deslocamento do ponteiro, o vínculo `(núcleo, lote)` intacto e contagens separadas (pops originais, fragmentos, fragmentos selecionados). (5) A ociosidade da trás não localiza o custo: o `other busy` sobe de 15,2% para 20,2%. (6) Contadores por núcleo continuam atômicos; conservação só com a execução parada | (1) Sim: `CollectT<true>` só troca as ordens de memória de `current` para `seq_cst` (`gpu_dirty_memory_manager.h:56-85`), e a prova da 3.3 assume essa ordem em todo acesso a `current`. No x86-64, CAS e `exchange` já são instruções com trava nas duas variantes, e a carga `seq_cst` é um `mov`: a economia perdida deve ser pequena (mapeamento padrão, não conferido no binário). (2) Sim, no caso parcial (`protocol.h:265-300`). O caso "sem nenhum registro" que eu acrescentei não procede: `protocol.h:695` já o torna ambíguo (corrigido na 3.7.3). Achado meu: a reciclagem por expiração (`ledger.h:166-201`) também perde um registro antes da aplicação dele, e cai no mesmo caso. (3) e (4) Sim. (5) Sim, conferido nos logs; achado meu: o diagnóstico reparte a aplicação em 2,6 vezes mais lotes (8.4). (6) Sim | 8.4 corrigido (custo distribuído, `at_risk` não confiável, A não mostrado como raro). Escopo do T3a.1 abaixo |
| 15 (revisão 5, sobre `d348ace`) | (1) T3a.1 sem bloqueador. (2) T3a.2 antes de T3a.3; T3a.3 condicional; o cursor de `Register` só avança no sucesso, e o estouro pode vir daí; "60 avisos por trecho" não prova o mesmo bloco. (3) O `blkcmp` compara com o payload **efetivamente enviado** no upload: não usar `DownloadBufferMemory` (ele mexe no rastreio e escreve no guest), não reler o guest depois, só declarar substituição se o caminho chegar ao upload; o upload pode passar à frente da cópia diagnóstica pela reordenação. (4) A versão síncrona, amostrada e limitada, serve para achar uma ocorrência; a perturbação deve ser declarada. A frente usa o rasterizador depois de `DrainBack`. (5) Seleção pseudoaleatória com semente, com denominadores; o resultado diz "bytes antigos da GPU diferentes do payload que os substituiu" | Sim, tudo conferido: o hook antes de `:148-150` e `:154-156`; o payload é o `tmp_buffer` (`buffer_cache.h:159-162`); `DownloadBufferMemory` em `:2145-2191`; a reordenação depende de `IsRegionUsed` (`vk_buffer_cache.cpp:455`), então um `MarkUsage` da cópia diagnóstica a impede; `puller.cpp:179-180`; `ledger.h:178`/`:232` | 8.5 corrigido (amostra não aleatória, causa provável do estouro, T3a.3 condicional). Escopo do T3a.1b e do T3a.2 abaixo |

**T3a.1b (decidido pelo planejador; pequeno, sem mudar o protocolo dos slots):** quando `Register` estoura, o cursor do
núcleo avança `kScan` posições, para o aviso seguinte examinar outros slots. O cursor é privado do escritor do
núcleo (π4) e não entra na prova. Um teste: 8 slots seguidos ocupados e não expirados, o resto livre; o primeiro aviso
estoura e o segundo registra fora deles.

**T3a.2 (`blkcmp`; escopo aprovado pelo revisor, revisão 5; desenho na seção 3.8, sem código):**
- **Parte nova `blkcmp`** de `EDEN_LIST_DIAG` (exige `blk`). Só considera faixas **afirmadas com `at_risk > 0`**, e
  sorteia entre elas com semente própria, de forma reproduzível (não "1 de cada M"). Tem também um orçamento por
  segundo e um teto de tamanho, ambos declarados.
- **Onde:** em `InlineMemoryImplementation`, depois de `SynchronizeBuffer` e **antes** da cópia de upload. Nenhum outro
  caminho: se `CachedWriteMemory` retorna antes (`:148-150`) ou vai para `WriteMemory` (`:154-156`), a faixa sorteada
  conta como descartada, "sem upload".
- **Primeira versão síncrona:** staging **próprio** de download; `MarkUsage` da região, para a reordenação não passar o
  upload à frente; cópia buffer → staging com barreira; `Finish`; cópia dos bytes antigos. **Nada** de
  `DownloadBufferMemory`, escrita no guest, `ClearDownload` ou mudança em `gpu_modified_ranges`. A referência é o
  `inlined_buffer` que o upload vai enviar, nos bytes `at_risk`. A perturbação (um `Finish` sob o `buffer_cache.mutex`
  por comparação) é declarada no relatório. A versão assíncrona, por fence, fica para depois, se a síncrona
  perturbar demais.
- **Thread:** a mesma que hoje grava o upload (a de trás, ou a da frente depois de um `DrainBack` completo,
  `puller.cpp:179-180`). Conta as duas separadamente.
- **Saída:** elegíveis, sorteadas, comparadas, descartadas por motivo (orçamento, sem upload, sem mapas de memória,
  tamanho); faixas iguais e diferentes; bytes comparados e diferentes. As primeiras diferenças vão ao log com
  endereço, lote, deslocamentos e amostra dos bytes. O texto do resultado: "bytes antigos da GPU diferentes do
  payload que os substituiu". Isso **não** prova corrupção visível, nem exclui uma escrita legítima concorrente.
- **Fora:** T3a.3 e a versão assíncrona.

| # | O que a crítica disse | Procede no código? | O que mudou |
| --- | --- | --- | --- |
| 16 (revisão 6, sobre `54e2381`) | (1) T3a.1b aprovado e encerrado. (2) **Bloqueador:** a captura depois de `SynchronizeBuffer` chega tarde. `InlineMemoryImplementation` faz `ClearDownload`, subtrai a faixa de `gpu_modified_ranges`, `FindBuffer` e `SynchronizeBuffer` (`:2116-2121`), e este último sobe as faixas da CPU fora de `gpu_modified_ranges` (`:2002-2014`), então pode sobrescrever os bytes `at_risk` antes da cópia diagnóstica. A frase "a posição não importa" da 3.8.1 está errada: guardar endereços não guarda valores. (3) `MarkUsage` e `reorder_unprotected` saem da versão síncrona e ficam como requisito da assíncrona. (4) Marca de thread própria, com `other/unknown`, sem supor "frente". (5) M=4, 5/s e 4096 B estão bem; declarar taxa sustentada e rajada do balde; o staging precisa caber o alinhamento das até 16 partes. Tirar o teste de sanidade que rejeita 100% de diferenças com `gpu_old` zerado | Sim: conferido em `buffer_cache.h:2002-2009` e `:2116-2121`. E o `FindBuffer` também não serve para a captura: ele pode chamar `SynchronizeBufferWrites` (`:1759-1761`) e criar buffer. O começo dele, `page_table[page]` + `IsInBounds` (`:1754-1758`), é consulta pura | Decisão do planejador, abaixo |

**T3a.2, revisão 6 (decidido pelo planejador):**
- **Ponto da captura:** a **primeira** coisa de `InlineMemoryImplementation`, antes de `ClearDownload`. A faixa sorteada
  é procurada só por consulta: `page_table[device_addr >> CACHING_PAGEBITS]` e `IsInBounds(device_addr, size)`. Nenhum
  `FindBuffer`. Sem buffer único que contenha a faixa, ela é descartada como `no_single_buffer`. Daí vêm o staging
  próprio, a cópia com barreira, o `Finish` e a cópia dos bytes antigos. Depois, o código de hoje segue **sem mudança**.
  O payload de referência continua sendo o `inlined_buffer`, que o chamador já preencheu.
- **Por que isso basta:** tudo o que o upload desta aplicação pode sobrescrever (`SynchronizeBuffer`, o próprio upload) é
  gravado **depois** do `Finish`. O que veio antes já está na fila e é produtor anterior.
- Fora da versão síncrona: `MarkUsage` e `reorder_unprotected`, que ficam escritos como requisito da assíncrona.
- **Thread:** marca `thread_local` própria do diagnóstico, posta onde a thread de trás e a da frente usam o
  rasterizador. Sem marca, a comparação conta como `other/unknown`.
- **Balde:** taxa sustentada de 5/s e rajada de até 5, as duas declaradas. Staging dimensionado pela soma das partes
  com o alinhamento delas.
- **Testes:** sai o teste de sanidade dos zeros; ficam os do harness.

**T3a.1 (escopo aprovado pelo revisor, revisão 4; desenho na seção 3.7, sem código):**
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
