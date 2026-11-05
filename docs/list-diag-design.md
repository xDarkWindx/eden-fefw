# T3: build de diagnóstico (`EDEN_LIST_DIAG`), desenho proposto

**Estado: proposta, aguardando aprovação. Nenhum código do emulador foi escrito.** Só o harness
(`tools/gpu-contract-harness/`, item 4) e os planos foram alterados. Escrito em 2026-11-05 sobre `740b932`
(branch `claude/elegant-wozniak-c2taum`), como parte de `docs/handoff-contracts-prototype.md`, T3.

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

O estado é um `std::atomic<u32>` com leitura relaxed (a mesma instrução de uma leitura comum), o que também
permite os testes ligarem e desligarem partes. Sem a variável: **uma leitura estática e um desvio por ponto de
observação**, nenhuma cópia, buffer ou trava nova (`handoff`, T3, item 1).

## 2. Arquivos e interfaces

| Arquivo | O quê |
| --- | --- |
| `video_core/list_diag.h` (novo) | `VideoCommon::ListDiag`: `Enabled(Part)`; o livro-razão dos avisos segurados (`HeldLedger`); `NoteHeldWrite`, `NotePop`, `NoteApply`; funções puras de contagem de bytes (interseção de faixas, bytes da GPU fora das escritas exatas); fila de eventos de log; para `list`: `ListEntryRecord`, `ClassifyWords`, `CompareWithCoherent`, a tabela "último escritor por página". Modelos de `RangeSet` como parâmetros, para os testes usarem os reais |
| `video_core/list_diag.cpp` (novo) | leitura da chave, estado global, rodízio de log, resumo final |
| `video_core/back_thread_stats.h/.cpp` | `ListDiagCounters` dentro de `Shared` e um bloco "list diag" no `Report()` (por segundo, janela de 4 s), como os demais contadores |
| `core/memory.cpp:765` | **1 ponto:** depois de `gpu_dirty_managers[core].Collect(address, size)`, `ListDiag::NoteHeldWrite(address, size)` (o core já inclui `back_thread_stats.h`, `memory.cpp:34`) |
| `video_core/buffer_cache/buffer_cache.h:719-722` | **1 ponto:** dentro do laço do pop, antes do `WriteBlockUnsafe`: `ListDiag::NotePop(start, end, bytes)` |
| `video_core/buffer_cache/buffer_cache.h:141` | **1 ponto:** no topo de `CachedWriteMemory` (antes do `return` de região não registrada e antes de qualquer mudança): `ListDiag::NoteApply(device_addr, size, gpu_modified_ranges, cpu_read_ranges, ...)` |
| `video_core/gpu.cpp` (`InvalidateGPUCache`) | descarregar o log (fora de todas as travas dos caches), limitado a 1 vez por segundo; resumo final no destrutor |
| `video_core/dma_pusher.cpp:140-165` (só `list`) | **1 ponto por entrada:** um registro de entrada e um `Defer` (ou chamada direta sem o split) |
| `video_core/buffer_cache/buffer_cache.h:1735` (só `list`) | `MarkWrittenBuffer`: atualiza a tabela do último escritor por página |
| `src/tests/video_core/list_diag.cpp` (novo), `src/tests/CMakeLists.txt` | testes (seção 8) |

Os cinco pontos de `pop`/`blk` são **uma chamada cada**. Nada muda no que os caches executam.

## 3. Observar sem interferir

### 3.1 O que o diagnóstico não pode fazer

- Nenhuma trava nova que **ordene** o pop em relação à aplicação dos avisos. O pop e a aplicação já são
  serializados pelo `buffer_cache.mutex` (o pop em `fence_manager.h:286-293`, a aplicação em
  `vk_rasterizer.cpp:972-981`, **verificado**); o diagnóstico não acrescenta aresta entre eles.
- Nenhuma aplicação antecipada, nenhum `GatherGPUDirtyMemory` na thread de fences, nenhum `Gather` extra: o
  `Gather` **consome** os avisos (`core/gpu_dirty_memory_manager.h:56-82`) e mudaria o que mede.
- Nenhuma espera. Nada do diagnóstico bloqueia o caminho de draws, o pop, a aplicação ou a escrita da CPU.

### 3.2 Como a thread de fences "lê o estado dos avisos" sem sincronização que mude a ordem

**Não lê o `GPUDirtyMemoryManager`.** Duas razões (**verificadas** no código):

1. Ele não tem leitura que não consuma: o `Gather` troca os buffers (`:61`) e o `current` (`:60`). Um "peek"
   teria de tomar o `guard` (`:34,59`), que a escrita da CPU também toma no caminho lento do `Collect`.
2. **Um peek erraria por falta.** Depois do `Gather` (`:56-82`) os avisos já saíram do manager, mas ainda **não
   foram aplicados**: a aplicação roda depois, em `OnCachedWrites` (`gpu.cpp:134-162`,
   `vk_rasterizer.cpp:958-992`), tomando o `buffer_cache.mutex` só ali. É exatamente um dos instantes em que o
   pop pode cair (entre o `Gather` e o `CachedWriteMemory`), e o manager já não o mostra.

**Livro-razão próprio, só do diagnóstico (`HeldLedger`).** Um registro por aviso segurado, com o intervalo
**exato** escrito pela CPU (que o `Collect` perde ao arredondar para 64 bytes, `:113-121`):

- **Escritor (CPU do jogo):** `NoteHeldWrite` depois do `Collect` real, num anel por núcleo de **um só
  escritor** (o `sys_core` já é serializado por `sys_core_guard`, `core/memory.cpp:709-717`). Só stores atômicos;
  nenhuma trava, nenhum `fetch_add` compartilhado. Cada slot usa uma sequência do tipo seqlock (par = publicado).
- **Pop e aplicação (leitores):** varrem os anéis **sem trava**, sob o `buffer_cache.mutex` que já seguram
  (o pop e o `CachedWriteMemory` já são mutuamente exclusivos por ele), e marcam o slot com stores atômicos
  (`t_pop`, `t_apply`). A ordem **escrita, pop, aplicação** sai da própria máquina de estados: o pop só marca
  um slot **pendente** (`t_apply == 0`), e a aplicação marca `t_apply` na mesma seção crítica em que lê o
  guest. Não precisa de contador global; os carimbos (`BackStats::Stamp()`, TSC) servem só para larguras de
  tempo.
- **Slots pendentes nunca são reaproveitados**, exceto por expiração (5 s, contada). Anel cheio: o aviso é
  descartado e contado (`held_overflow`).

**O que isso pode deixar de ver** (todos contados, para o resultado não parecer mais completo do que é):

| Ponto cego | Efeito | Contador |
| --- | --- | --- |
| O registro vem **depois** do `Collect` real: por alguns ns o aviso existe nos caches e não no livro-razão | um pop nesse intervalo não é contado (subconta) | (não observável; limite pequeno, **hipótese**) |
| Anel cheio, ou slot expirado sem ser aplicado | aviso fora do livro-razão | `held_overflow`, `held_expired` |
| O `memcpy` no guest vem **depois** de o aviso ser registrado (`core/memory.cpp:587-590`, `:649-653`) | um pop entre o registro e o `memcpy` parece janela e é benigno | separado na aplicação: `window_newer` (seção 4) |
| Valor igual: a CPU escreveu os mesmos bytes que o pop gravou | a confirmação por comparação diz "sobrescrito" sem haver perda real | `window_overwritten` é "**consistente com perda**", não "perda" |
| Escritas da CPU que não passam pelo caminho de aviso segurado (páginas sem dados da GPU: `WriteMemory`, `buffer_cache.h:132-138`; escritas de serviços HLE) | fora do problema medido | não entram |
| Pop de outros caches (textura, queries) | não instrumentados | listado em "fora de escopo" |
| Carimbos TSC entre núcleos | larguras de tempo com ruído de dezenas de ciclos | só soma/máximo, nunca ordem |

**Perturbação que sobra** (dita, não eliminada): um store atômico por aviso na thread da CPU, uma varredura
dos slots pendentes por faixa de pop e por faixa aplicada (sob o mutex que já está tomado), o que **alonga
levemente** as duas seções críticas (a de aplicação e a do pop) e pode deslocar a probabilidade da janela, mas
não impõe ordem entre elas. A medição de custo da seção 8 mede isso.

**Alternativa descartada (menos recomendada):** mutex folha do diagnóstico no registro. Não ordena o pop contra
a aplicação (não é tomado dentro de nenhuma outra trava, nem toma nenhuma), mas põe uma aresta nova na thread da
CPU do jogo, que hoje só tem `guard` no caminho lento. Fica como plano B se o anel sem trava se mostrar
complicado demais.

## 4. Problema A: janela do pop (contadores e logs próprios)

**Definição do evento** (o que o item 1a pede): *um download antigo publicado sobre bytes que têm um aviso de
escrita segurado ainda não aplicado.* Registrados: a **faixa do download** (a faixa que o pop grava: `start`,
`end` do laço `buffer_cache.h:719-722`), a **faixa da escrita** (o intervalo exato do livro-razão) e a **ordem
dos eventos** (escrita, pop, aplicação) com as larguras em µs.

**Confirmação por comparação (barata, só quando há sobreposição):** o pop guarda um hash (FNV-1a) de até 64
bytes da interseção que **ele gravou** (o conteúdo do download). Na aplicação, o hook lê do guest os mesmos
bytes (`device_memory.ReadBlockUnsafe`, o mesmo que `CachedWriteMemory` faz em `:154`) e compara:

- igual: os bytes que a aplicação vai subir **são os do download**: consistente com a perda da escrita nova
  (`window_overwritten`);
- diferente: há bytes mais novos no guest (o `memcpy` veio depois do pop, ou outra escrita): benigno
  (`window_newer`);
- não leu (faixa não legível, amostra vazia): `window_unverified`.

| Contador (`Shared.list_diag`, relaxed) | O que conta |
| --- | --- |
| `held_registered`, `held_applied`, `held_expired`, `held_overflow` | avisos no livro-razão |
| `pop_ranges` | faixas que o pop gravou no guest (denominador) |
| `pop_ranges_over_held` | faixas de pop que cruzam ao menos um aviso pendente |
| `pop_over_held_bytes` | bytes da interseção |
| `pop_over_held_records` | avisos distintos atingidos por algum pop |
| `window_overwritten`, `window_newer`, `window_unverified` | desfecho na aplicação (acima) |
| `window_write_to_apply_us` (soma, máx, n) | largura da exposição: da escrita à aplicação, de **todo** aviso aplicado (dimensiona a janela mesmo sem ocorrência) |
| `window_write_to_pop_us` (soma, máx, n) | da escrita ao pop, nos atingidos |

**Log** (rótulo `[ListDiag][pop]`), uma linha por aviso atingido, emitida na aplicação (quando a tríade está
completa); um aviso atingido que expira sem aplicação sai com `apply=never`:

```
[ListDiag][pop] write=[0x..,+N) download=[0x..,0x..) overlap=N order=write@t0 -> pop@+Aus -> apply@+Bus guest_at_apply=eq_pop|newer|unverified
```

## 5. Problema B: blocos de 64 bytes (contadores e logs próprios, separados do A)

**Definição:** *a aplicação de um aviso segurado cujo bloco de 64 bytes cobre bytes ainda em
`gpu_modified_ranges`* (`core/gpu_dirty_memory_manager.h:93-96`; `buffer_cache.h:141-158` e `2105-2128`). O hook
roda no topo de `CachedWriteMemory`, **antes** de `InlineMemoryImplementation` subtrair o trecho de
`gpu_modified_ranges` (`:2107-2108`), então vê o estado que a subtração vai destruir.

Registrados: **endereço e tamanho do trecho aplicado** (é o bloco, ou a união de blocos da mesma página depois
da junção em `gpu.cpp:138-161`, que é o que o cache enxerga), as **escritas exatas** da CPU dentro dele (do
livro-razão) e os **bytes da GPU cobertos**: `gpu_modified_ranges ∩ trecho`, e, subtraindo as escritas exatas,
os **bytes da GPU fora do que a CPU escreveu**, que são os bytes em risco (o upload do bloco os troca no host
pelos bytes do guest, e a subtração impede que algo os devolva). Em Switch 2, com `!AsyncBufferDownloads()`,
separa os que a CPU já leu (`cpu_read_ranges`, `buffer_cache_base.h:541-562`) dos que **só existem no host**.

Um limite que o contador diz de si mesmo: "em risco" é o **limite superior** da perda. Só há perda se o valor
da GPU difere do que o guest tem naqueles bytes, e o diagnóstico não lê o buffer do host (isso é a parte `list`,
e só para entradas de lista).

| Contador | O que conta |
| --- | --- |
| `blk_applies` | trechos aplicados por `CachedWriteMemory` |
| `blk_applies_over_gpu` | os que cruzam `gpu_modified_ranges` |
| `blk_bytes_range`, `blk_bytes_exact` | bytes do trecho; bytes escritos de fato (livro-razão) |
| `blk_gpu_bytes_covered` | `gpu_modified_ranges ∩ trecho` |
| `blk_gpu_bytes_at_risk` | a parte fora das escritas exatas |
| `blk_at_risk_cpu_read`, `blk_at_risk_host_only` | em Switch 2: já lidos pela CPU / só no host (com `AsyncBufferDownloads()`, ficam `unknown`) |
| `blk_no_exact` | trechos sem escritas exatas no livro-razão (overflow ou expiração); a conta usa só `covered` |

**Log** (`[ListDiag][blk]`):
`range=[0x..,+N) blocks=N/64 exact=[(0x..,n)...] gpu_covered=C at_risk=R (cpu_read=a host_only=b)`.

**Independência:** `blk` não usa o pop nem os campos `t_pop`; só o livro-razão (para as escritas exatas) e o
estado do cache. Pode rodar sem `pop`.

## 6. Entradas de lista (`list`: as letras a a d do item 2 do T3)

Só liga com `list` (ou `1`). **Não muda o que é executado.** A frente nunca consulta caches nem o scheduler
(`docs/architecture-plan.md`, 3.3): o que ela faz é montar um registro e enfileirar um pedido no ring.

1. **Registro da entrada (frente, `DmaPusher::Step`).** Depois de escolher as palavras (snapshot, memória
   atual sem flush, ou com flush: `dma_pusher.cpp:141-165`), copia as palavras usadas para um registro do
   diagnóstico (a frente só faz `memcpy` e carimbo), com: fonte (`snapshot`, `resumed_unsafe`,
   `no_snapshot_unsafe`, `safe`), deslocamento de retomada, endereço, estado do `dma_state` no início da entrada e
   a **última espera de host** vista pelo puller (tipo, valor, posição), guardada pelo puller só com a parte
   ligada. O registro vai por `GpuSplit::Defer` (`gpu_split.h`, "Runs `func` on the back after everything sent
   before it"), ou direto sem o split.
2. **Pedido ordenado (trás).** O pedido roda na posição da entrada. Confere em `gpu_modified_ranges` se alguma
   página da entrada tem bytes da GPU; se tem, grava um `CopyBuffer` GPU → **buffer privado do diagnóstico**
   (padrão do `CommitAsyncFlushesHigh`, `buffer_cache.h:672-687`: staging mapeado, `PreCopyBarrier`,
   `PostCopyBarrier`) só das faixas da GPU, e anota `scheduler.CurrentTick()`
   (`vk_master_semaphore.h:32`). **Não baixa para o guest e não mexe em `gpu_modified_ranges`.**
3. **Conclusão por tick, sem esperar.** `IsFree(tick)` (`vk_master_semaphore.h:42`) é consultado depois, sem
   bloquear (a cada `InvalidateGPUCache`); quando livre, lê o staging, compara com as palavras da frente e
   registra: deslocamento, classe (cabeçalho, dado de método, parâmetro de macro: `ClassifyWords`, uma segunda
   passada **fora** do laço de `ProcessCommands`, para o laço quente não ganhar desvios), método e subcanal, tipo
   e posição do último produtor da faixa (a tabela por página, abaixo) e a última espera de host. A conclusão é
   a **da própria cópia**, nunca o valor do syncpoint (decisão 4 do handoff).
4. **Escrita perdida.** A tabela "último escritor por página" (atualizada em `MarkWrittenBuffer`, só com a
   parte ligada) guarda `{serial, tipo}`. A frente carimba o `serial` já gravado pela trás no instante em que
   leu a entrada (uma leitura atômica, sem espera). No pedido, a trás confere as páginas da entrada: serial
   maior que o carimbado = um produtor enviado antes da entrada e gravado **depois** de a frente ler. Conta
   mesmo com a página sem marca na leitura.
5. **Pontos cegos (item d).** O diagnóstico lista, por observação, os produtores que **não** passam por
   `MarkWrittenBuffer` e que estão dentro de `video_core/`: query/semáforo (`query_cache.h:285-310`), release de
   QMD (`kepler_compute.cpp:169-174`), DMA no caminho lento (`maxwell_dma.cpp:143-149`), `BufferClear` e upload
   inline (`buffer_cache.h:283-304`, `vk_rasterizer.cpp:1220-1269`) e o reset indireto na CPU
   (`vk_rasterizer.cpp:260-332`). São seis pontos de uma chamada (`NoteForeignWrite(tipo, addr, size)`) que
   alimentam a mesma tabela por página, com tipo próprio. **Não cobre** (dito no relatório): imagens e render
   targets e seus aliases (P6), as escritas de HLE (decodificador, VIC, vi: P13 a P15, fora de `video_core/`), P4
   sem origem da GPU e P1 além dos bytes escritos (inventário, 3.1).

| Contador | O que conta |
| --- | --- |
| `list_entries` (por fonte) | entradas vistas |
| `list_candidates` | entradas em página com bytes da GPU |
| `list_copies`, `list_copies_skipped_cap` | cópias gravadas; puladas pelo teto |
| `list_compared`, `list_differing` | cópias lidas; com diferença |
| `list_diff_words` (por classe) | palavras diferentes: cabeçalho, dado de método, parâmetro de macro |
| `list_missed_writes` (com e sem espera de host antes) | produtor gravado depois da leitura da frente, na página da entrada |
| `foreign_writes` (por tipo) | produtores fora de `MarkWrittenBuffer` que cruzaram uma página de entrada |

**Perturbação, dita:** barreiras e cópias a mais no fluxo mudam o tempo (`docs/gpu-produced-data-plan.md`,
4.1.2). Por isso `list` é medida **à parte** das partes `pop` e `blk`.

## 7. Declaração da seção 2.2 (T3)

| | |
| --- | --- |
| Condição nova | Observação: `EDEN_LIST_DIAG`. Nenhuma condição de execução nova |
| Esperas novas | **Nenhuma.** Nada do diagnóstico bloqueia pop, aplicação, escrita da CPU, frente ou trás |
| Recursos retidos | `pop`/`blk`: anel fixo de slots (4 núcleos × 1024 slots × 64 B = 256 KiB), alocado só com a parte ligada. `list`: buffer privado de staging e as cópias em voo, **com teto** (padrão: 16 MiB em voo e 64 entradas; acima, a entrada é pulada e contada); a tabela por página (2^16 entradas × 16 B = 1 MiB) |
| Custo sem uso | uma leitura relaxed de flag e um desvio por ponto: `core/memory.cpp:765` (só no caminho de escrita segurada), o laço do pop (por faixa), o topo de `CachedWriteMemory`, `MarkWrittenBuffer`, `DmaPusher::Step` (por entrada) e os seis pontos de `NoteForeignWrite`. **A medir** (seção 8) |
| Caminhos afetados | thread da CPU do jogo (1 store atômico por aviso segurado); thread de fences e quem aplica (uma varredura sob o `buffer_cache.mutex` já tomado); a frente do split **só** monta o registro e usa o ring (nunca consulta caches nem o scheduler) |
| Teste | seção 8 |

## 8. Testes

### 8.1 Integração (Catch2, `src/tests/video_core/list_diag.cpp`)

Contra as classes **reais** onde o código permite; o resto por funções puras que o emulador chama do mesmo
jeito (como `pending_download.cpp` faz com `RetainPendingImageDownloads`). Instanciar um `BufferCache<P>`
inteiro com um runtime falso é grande demais para o T3; por isso as decisões ficam em funções puras que recebem
os `RangeSet`.

| Teste | Reais | O que confere |
| --- | --- | --- |
| Aviso em bloco de 64 bytes | `Core::GPUDirtyMemoryManager` (`Collect`, `Gather`), `Common::RangeSet` | O `Collect` de 4 bytes volta do `Gather` como **64 bytes alinhados** (hipótese 7.2 contra o código real) e `blk_gpu_bytes_at_risk` = 60 com 64 bytes da GPU no bloco; sem bytes da GPU, 0 |
| Escrita exata dentro do bloco | idem | `at_risk` desconta só as escritas exatas; dois avisos na mesma página, depois da junção de `gpu.cpp:138-161` |
| Janela do pop, ordem **escrita → pop → aplicação** | `Common::OverlapRangeSet` como `async_downloads`, livro-razão | `pop_ranges_over_held` = 1, tríade registrada, `window_overwritten` |
| Pop **antes** da escrita | idem | nenhum evento |
| Aplicação **antes** do pop | idem | nenhum evento (o aviso já saiu do livro-razão) |
| Pop entre o registro e o `memcpy` | idem, guest falso | `window_newer` (benigno) |
| Aviso entre o `Gather` e a aplicação | `GPUDirtyMemoryManager` real | o pop **conta**: o aviso já não está no manager e continua pendente no livro-razão |
| Anel cheio, expiração | livro-razão | `held_overflow`, `held_expired`; nenhum evento falso |
| `ClassifyWords` e a comparação | funções puras | cabeçalho, dado de método e parâmetro de macro; entrada retomada com `dma_state` no meio |
| **Desligado: nada muda** | todos | com `EDEN_LIST_DIAG` desligado, os mesmos roteiros produzem a mesma saída e **nenhum contador novo é incrementado** (`Shared.list_diag` inteira em zero), e nenhuma alocação do livro-razão |
| Concorrência | threads reais | escritor, pop e aplicação em threads, sem trava no escritor; compilado à parte com TSan na nuvem (`g++ -std=c++20 -fsanitize=thread`), como os harnesses do split (`docs/back-thread-split-plan.md`, 8.1) |

O `DmaPusher` com rasterizador falso e o `puller` reais para a parte `list` são **a confirmar** na
implementação: se houver uma forma pequena de instanciá-los (os testes `native_semaphore.cpp` e
`indirect_reset.cpp` podem servir de ponto de partida), os casos dirigidos do harness (primeira escrita,
retomada dentro da entrada, reuso pela CPU, escrita parcial de página, dois produtores, remapeamento) entram
ali; senão ficam só no harness e o teste de integração cobre `ClassifyWords`, a comparação e a tabela por página.

### 8.2 Verificação na nuvem

`tools/gpu-contract-harness/run.sh` (três builds) e `g++ -std=c++20 -fsyntax-only -Wall` dos arquivos
alterados (método de `docs/handoff-aggressive.md` e `docs/perf-handoff.md`). A cadeia completa de build do
emulador não roda na nuvem.

### 8.3 Plano de teste no FE (usuário, em casa)

Orçamento: `docs/architecture-plan.md`, 12.1 (até 5% de draws/s por item). **Estatísticas de tempo desligadas**
(`EDEN_BACK_THREAD_STATS` ausente) nas medições de draws/s, para o diagnóstico não medir a si mesmo; o resumo
final do diagnóstico sai no log de qualquer forma (seção 2).

| Rodada | Configuração | Onde | O que sai |
| --- | --- | --- | --- |
| A | sem `EDEN_LIST_DIAG` | praça e palácio do FE | draws/s de referência; **repita A duas vezes** para o ruído |
| B | `EDEN_LIST_DIAG=pop,blk` | praça e palácio | draws/s (custo do livro-razão e dos hooks); contadores no resumo final |
| C | `EDEN_LIST_DIAG=1` | praça e palácio | draws/s com as cópias (custo da parte `list`) |
| Bananza | `EDEN_LIST_DIAG=pop,blk` e depois `=1` | onde o chão some | contadores e logs das seções 4 a 6; a hipótese das entradas candidatas |

Alternar A/B/A/B no mesmo build, mesmo lugar e mesma câmera, PowerShell novo por rodada (como a matriz do
handoff, seção 7). Registrar **também** o número observado em degraus de FPS (`docs/architecture-plan.md`,
12.1). O que se compara é o custo do diagnóstico, não a correção: uma rodada com zero ocorrências **não**
invalida a janela nem os blocos (seção 0), e uma rodada com ocorrências dimensiona exposição, não decide o
mecanismo.

## 9. O que fica de fora

- **Correções** (T4): nenhuma direção da seção 12 do plano, nenhuma mudança de política de fence.
- **Pop do texture cache e do query cache** (a seção 12 do plano diz que não foram analisados): não
  instrumentados.
- **Escritas de HLE** (decodificador, VIC, vi), **imagens, render targets e aliases**, P4 sem origem da GPU: não
  vistos pelo filtro nem pela tabela por página; saem no relatório como "não cobertos".
- **OpenGL** e o ramo Android do upload (`buffer_cache.h:2008-2028`).
- **Efeitos de tempo do Vulkan** (barreiras, várias filas, caches da GPU): o diagnóstico não os mede.
- **Uma terceira intercalação, lida e não pedida:** o aviso segurado é registrado **antes** do `memcpy`
  (`core/memory.cpp:587-590`, `:649-653`); se a aplicação lê o guest entre os dois, o upload leva bytes antigos e
  nenhum aviso novo corrige isso (**hipótese**, só leitura). Medi-la exigiria um hook **depois** do `memcpy`, no
  caminho quente de toda escrita da CPU. Não incluo sem sua aprovação.

## 10. Decisões que preciso de você antes de escrever código no emulador

1. **Fases.** Proponho: **T3a** (`pop` e `blk`: livro-razão, 3 hooks, contadores, logs, testes) primeiro,
   porque responde a pergunta que a seção 12 do plano deixou aberta para o T4 com custo mínimo e sem cópias na
   GPU; **T3b** (`list`) depois, com seu resultado de custo. Aprovado?
2. **Livro-razão sem trava (recomendado) ou mutex folha** (seção 3.2)?
3. **Hooks de produtores estrangeiros** (seção 6, item 5): os seis dentro de `video_core/` na T3b, ou deixar
   todos para depois?
4. **Limites:** primeiras 32 ocorrências de cada tipo, depois 1 a cada 64, teto de 512 linhas por sessão;
   cópias em voo até 16 MiB. Servem?
5. **A terceira intercalação** (seção 9, último item): entra como contador, com o hook depois do `memcpy`, ou
   só fica registrada?
