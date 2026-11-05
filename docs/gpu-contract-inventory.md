# Inventário dos contratos: consumidores, produtores, mecanismos, efeitos do High e apresentação

Escrito em 2026-11-05 sobre `61d936e` (branch `claude/elegant-wozniak-c2taum`). É o T2 de
`docs/handoff-contracts-prototype.md`: só documentação, nenhuma mudança de código. Nada foi rodado; tudo vem da
leitura do código deste fork.

Graus de certeza, como nos outros planos: **verificado** (aberto e lido nesta sessão, com `arquivo:linha`),
**medido** (número de um doc ou commit, dizendo de quem) e **hipótese**. Onde uma linha cita código que não
abri, ela diz isso. Todos os caminhos são relativos a `src/`, salvo indicação.

Threads, nesta ordem de uso:

- **Frente:** a thread da GPU (`gpu_thread.cpp`), que com o split ativo só interpreta as listas de comandos
  (`gpu_thread.cpp:73-98`). **Trás:** a thread do split (`gpu_split.cpp:93`), dona dos caches e do rasterizador.
  Sem o split (`EDEN_GPU_SPLIT=0`, ou nível/DMA que o proíbem, `gpu_split.cpp:77-90`), uma thread só faz as duas
  coisas (`gpu_thread.cpp:100-107`).
- **CPU do jogo:** as threads de CPU emulada, que chamam `OnCPURead` e `OnCPUWrite`.
- **HLE:** threads de serviço (decodificador, vi, nvdrv).

Contratos (`docs/architecture-plan.md`, 2.2): **Memória** (que versão dos bytes, até quando vale),
**Sincronização** (condição, quem satisfaz, limite) e **Apresentação** (pronto, exibível, liberado).
Princípio 6 do mesmo plano: **comando** é o que muda estado que persiste (listas, parâmetros de macro,
argumentos indiretos); só comando exige modo exato. **Dado** pode aceitar uma concessão declarada.

## 1. Resumo

1. **Nenhum consumidor de comandos lê bytes que a GPU produziu com versão definida hoje.** A lista de
   comandos vem do snapshot da submissão (`nvhost_gpu.cpp:392-406`), da memória atual sem flush
   (`dma_pusher.cpp:160-164`) ou da memória atual com flush, só com uma thread (`:156-159`). Só a terceira
   baixa dados da GPU, e só os que os caches de buffers e de texturas conhecem (verificado).
2. **O filtro de páginas do plano (bitmap marcado em `MarkWrittenBuffer`) veria os storage buffers de draws e
   compute, a cópia DMA buffer-para-buffer (quando a origem foi escrita pela GPU) e a cópia imagem-para-buffer.**
   Não veria imagens e render targets, queries e semáforos, a cópia DMA que cai no caminho lento, os uploads
   inline, as escritas de HLE (decodificador, VIC, vi) e o reset indireto feito na CPU. Esses são os pontos
   cegos (seção 3.1).
3. **Dos quatro mecanismos provados pelo T1, dois existem em forma parecida e dois não existem** (seção 4):
   segurar escritas da CPU existe, mas sobre páginas modificadas pela GPU, não sobre entradas submetidas e não
   buscadas; o upload por palavra existe no caminho de segurar e, de outra forma, no de upload (subtração das
   faixas exatas da GPU). A espera real do acquire de syncpoint não existe, e a validade de transferências no
   remapeamento não existe porque não há transferências.
4. **Resposta à pendência da seção 11 do plano de arquitetura:** o upload de uma página marcada pela CPU **não**
   sobrescreve os bytes da GPU no mesmo buffer, no caminho que roda fora do Android: o upload subtrai de cada
   faixa as faixas exatas escritas pela GPU (`buffer_cache.h:1993-2000`). Ressalvas na seção 4.3.
5. **O valor "host" do syncpoint não sobe na conclusão da GPU em Fast.** Com o comportamento de fence padrão e
   o nível Fast, `IncrementHost` roda no momento em que a fence é sinalizada (`fence_manager.h:123-130`).
   Só nos modos de fence adiados (nível High, `Balanced` ou `Accurate`) ele espera a fence completar. Isso
   corrige uma frase do plano de listas e muda o que o harness do T1 modela como "valor do host"
   (seção 7, achado F1).
6. **Apresentação:** o fork protege só as palavras das listas contra o "frame à frente" (snapshot), limita a um
   composite em voo e libera quadros do decodificador pelo valor **guest** do syncpoint, com espera de 100 ms
   e escrita mesmo assim (seção 6). Dados que não são comandos não têm proteção equivalente (hipótese).

## 2. Consumidores de dados que a GPU produz

"Contrato" é o que deveria atender o consumidor segundo a seção 2.2 do plano de arquitetura. "Tipo" segue o
princípio 6.

| # | Consumidor | Caminho hoje (verificado) | Thread | Tipo | Contrato |
| --- | --- | --- | --- | --- | --- |
| C1 | Busca de lista: snapshot da submissão | `dma_pusher.cpp:146-155` usa `submitted_words`; o snapshot é lido com `ReadBlockUnsafe` na submissão (`nvhost_gpu.cpp:395-407`), só em canais de ABI nativa (`:395`). Só vale se `command_resume_offset == 0` (`:146`) e se, com leitura segura, a memória não está suja (`:148-149`) | Frente (split) ou única | Comando | Memória (hoje: nenhum; é a versão "da submissão", sem produtores) |
| C2 | Busca de lista: memória atual sem flush | `dma_pusher.cpp:160-164`, `GpuGuestMemory<..., UnsafeRead>`. Entra para entrada retomada depois de acquire (`command_resume_offset != 0`), para canal sem snapshot e quando o split impede a leitura segura (`:142`) | Frente (split) ou única | Comando | Memória |
| C3 | Busca de lista: memória atual com flush | `dma_pusher.cpp:156-159`, `SafeRead`; condição em `:142-143`: split inativo e (DMA Default com GPU High, ou DMA Safe). Com `SafeRead`, `core/guest_memory.h:128-129,141-142` chama `FlushRegion`/`ReadBlock`, que baixam texturas e buffers (`memory_manager.cpp:409-441`, `vk_rasterizer.cpp:841-860`) | Única | Comando | Memória |
| C4 | Busca de lista: lista de prefetch | `dma_pusher.cpp:101-108`. A lista é montada pelo HLE (`nvhost_gpu.cpp:322-357`, empurrada em `:421-449`): acquire, incremento e WFI do host, não palavras produzidas pela GPU | Frente ou única | Comando (feito pelo host) | Sincronização (conteúdo); Memória não se aplica |
| C5 | Parâmetros de macro (Maxwell3D) | `maxwell_3d.cpp:236-256` guarda as palavras e o endereço de cada segmento; `dma_pusher.cpp:121-138` marca `current_dirty` com `IsMemoryDirty` (no split, adiado para a trás, `:129-134`); `RefreshParametersImpl` relê com `ReadBlock` seguro (`maxwell_3d.cpp:266-281`), só com a parte `MacroParameters` ou, em Switch 2, com `NativeMacroRefresh` (`accuracy_parts.h:36-45`). Chamado antes de cada macro (`macro.cpp:106-380,1346,1388`) | Trás, ou frente com "macros na frente" (`gpu_split.cpp:102-103`: só sem `NativeMacroRefresh` e sem a parte) | Comando | Memória |
| C6 | Leitura FIFO de macro (args que a macro busca na memória) | `maxwell_3d.cpp:258-264` (`ReadBlock` seguro). Nativa na trás: `macro.cpp:1357-1359`. Macros na frente: `split.Drain` e depois a leitura (`maxwell_3d.cpp:1037-1040`) | Trás, ou frente depois de esvaziar a trás | Comando (decide draws) | Memória |
| C7 | Argumentos de draw indireto e contagem | Endereços resolvidos em `buffer_cache.h:1596-1615`; `BindHostDrawIndirectBuffers` chama `SynchronizeBuffer` (`:946-956`) e o draw consome o buffer do host na GPU (`vk_rasterizer.cpp:444-499`). A CPU não lê esses bytes | Trás | Comando (consumido na GPU) | Memória (uploads) e ordem do fluxo (barreiras: hipótese, não aberto) |
| C8 | Dispatch indireto | `vk_rasterizer.cpp:760-778`; `ObtainBuffer(..., FullSynchronize, DiscardWrite)` (`buffer_cache.h:336-340`) esquece o download desses bytes | Trás | Comando (consumido na GPU) | Memória |
| C9 | Reset indireto do Bananza feito na CPU | `vk_rasterizer.cpp:260-332`: `ReadBlock` seguro do descritor (`:287`), `FlushRegion` se o buffer está sujo (`:298-300`), `ReadBlockUnsafe` da lista de ponteiros (`:302`) | Trás | Dado que decide um substituto da GPU | Memória |
| C10 | Acquire de semáforo, canal nativo | `puller.cpp:184-243`. `DrainBack` antes da leitura (`:186`); `ReadNativeSemaphoreWord` baixa o buffer se `MustFlushRegion` (`:34-49`); se não satisfaz, `ReleaseFences(true)` e nova leitura (`:222-227`); persistindo, o canal é suspenso (`:234-243`), retomado em `CanResumeNativeAcquire` com novo `DrainBack` (`:145-154`). Sem limite de tempo | Frente (com a trás esvaziada) | Condição de sincronização | Sincronização, com Memória para o valor |
| C11 | Acquire de semáforo, canal legado | `puller.cpp:187-270`: dentro de `do { ... } while (false)`, cada `continue` (`:253,260,265`) salta para a condição `false` e **sai sem esperar** (leitura do código, não executado). `ProcessSemaphoreAcquire` (`:284-297`) espera sem limite (`acquire_timeout` é TODO, `:293`) | Frente | Condição de sincronização | Sincronização |
| C12 | Acquire de syncpoint | `puller.cpp:126-132`: só agenda `ReleaseFences`; a espera está comentada. Não satisfaz nada | Trás (`OnBack`) | Condição de sincronização | Sincronização |
| C13 | Descritor de QMD nativo | `kepler_compute.cpp:153-181`: `ReadBlock` seguro "por causa de patches da GPU" (`:162-164`) | Trás | Comando | Memória |
| C14 | Condição de renderização condicional | `maxwell_3d.cpp:626-645`, `ReadBlock` seguro; no host, `vk_query_cache.cpp:1407-1448` compara no buffer | Trás | Dado que decide draws | Memória |
| C15 | Leitura da CPU do jogo | `core/memory.cpp:685-701` → `gpu.cpp:257-272` → `GetFlushArea` (`vk_rasterizer.cpp:883-908`): registra a leitura (`NoteCpuRead`, `buffer_cache.h:733-743`) e, para buffers, devolve `preemtive = true` sem baixar. Para texturas modificadas pela GPU, a primeira leitura não é preemptiva (`texture_cache.h:662-685`) e baixa pelo `RequestSyncOperation` (`gpu.cpp:263-270`). Em canais de Switch 2 só se copia de volta o que a CPU leu (`buffer_cache_base.h:541-562`) | Thread da CPU do jogo, esperando a GPU para texturas | Dado (concessão `learned`, seção 12.3 do plano) | Memória |
| C16 | Upload de buffer | `buffer_cache.h:1969-2028` (`SynchronizeBuffer`): copia do guest as páginas marcadas pela CPU, menos as faixas da GPU | Trás | Dado | Memória |
| C17 | Upload de textura | `texture_cache.h:1181-1200`: `UnsafeRead` do guest (sem flush); no caminho acelerado, `ReadBlock` com `NoTextureCache` (`:1185-1187`); a fila assíncrona usa `ReadBlock` (`:1497`) | Trás | Dado | Memória |
| C18 | Leituras do motor DMA | `maxwell_dma.cpp:216,280,334` (`SafeRead`); `engine_upload.cpp:101-108` | Trás | Dado | Memória |
| C19 | Apresentação de buffer nativo | `vk_rasterizer.cpp:1271-1288`: sem imagem para o quadro, baixa as escritas da GPU antes do desswizzle da CPU | Trás | Dado | Apresentação |
| C20 | Descritores, constantes e ambiente de shader | TIC/TSC: `maxwell_3d.cpp:737,745`, `kepler_compute.cpp:338,346` (`UnsafeRead`), `shader_environment.cpp:378` (`ReadBlock`); constantes: `:817`, cópia `:874` (`UnsafeRead`) | Trás | Dado (hipótese: pode ser produzido pela GPU; não abri o uso) | Memória |
| C21 | Bit de sync da entrada | `dma_pusher.cpp:114-119,173-183`: com "Sync Memory Operations", `Drain` e `SignalFence` e a frente espera | Frente | Condição de sincronização | Sincronização |

Observações:

- **Já baixam o que a GPU escreveu antes de ler:** C3, C5 (quando a releitura está ligada), C6, C9, C10, C13,
  C14, C15 e C18. **Leem sem flush:** C2, C17 e C20 (`UnsafeRead`), e C1 por construção (o snapshot é de antes
  do produtor). A decisão "sujo ou não" usa `IsMemoryDirty`, que só vê buffers sem a parte `TextureFlush`
  (seção 5, E2).
- **O que não existe para nenhum comando** é a versão definida na posição da entrada (a cópia ordenada do
  protocolo): C5, C6 e C13 releem a memória **no momento da execução**, não na posição da entrada no fluxo.
- **C7 e C8 não leem na CPU:** o risco deles é ordem na GPU e upload de bytes escritos pela CPU, não versão de
  bytes da GPU.

## 3. Produtores que escrevem memória do jogo ou buffers do host

"Marca" diz se o produtor passa por `MarkWrittenBuffer` (`buffer_cache.h:1728-1738`: marca os bits de GPU do
rastreio, soma a faixa a `gpu_modified_ranges` e enfileira o download) ou por equivalente. O filtro de páginas
do plano seria marcado nesse mesmo ponto (`docs/architecture-plan.md`, 3.2).

| # | Produtor | Caminho (verificado) | Marca? | O filtro o veria? |
| --- | --- | --- | --- | --- |
| P1 | Storage buffers de draws gráficos | `buffer_cache.h:1261` (`BindHostGraphicsStorageBuffers`), multi-range `:1229`, texture buffers `:1283`, transform feedback `:1321`. A faixa marcada é a do binding inteiro, não só os bytes escritos (`:1259-1261`) | Sim | Sim (com falsos positivos) |
| P2 | Storage buffers e texture buffers de compute | `buffer_cache.h:1401` (com `needs_sync`), `:1424` | Sim | Sim |
| P3 | Compute com ponteiros globais (memória global) | O recompilador escolhe, entre os buffers de storage ligados, o de base mais próxima abaixo do endereço (`shader_recompiler/backend/spirv/spirv_emit_context.cpp:974-980`); a escrita cai em um buffer ligado como escrito, então passa por P1/P2. Ponteiro sem buffer correspondente não escreve | Sim, só se o buffer certo estiver ligado como escrito | Sim, só nesse caso |
| P4 | DMA buffer-para-buffer acelerado | `buffer_cache.h:222-281` (`DMACopy`): espelha as faixas GPU da origem no destino (`:258-264`) e marca os bits (`:270`) | Sim, **só se a origem tinha dados da GPU** | Parcial |
| P5 | DMA imagem-para-buffer | `vk_rasterizer.cpp:1393-1396` com `MarkAsWritten` (`buffer_cache.h:332-335`) | Sim | Sim |
| P6 | Imagens e render targets | `texture_cache.h:2521-2524`, `PrepareImage(..., is_modification)` (`:2593-2595`), render targets em `:488-509`, storage images em `renderer_vulkan/pipeline_helper.h:557`; aliases copiados em `SynchronizeAliases` (`texture_cache.h:2527-2577`). A marca é o flag `GpuModified` da imagem, não os buffers | Equivalente de imagem | **Não** (ponto cego) |
| P7 | Queries e semáforos de `Query` | `query_cache/query_cache.h:285-310`: `memcpy` direto na memória do jogo, na fence (`SignalFenceCoalesced`) ou na hora; o resultado das queries é copiado para buffers por `ObtainCPUBuffer(..., DoNothing)` (`vk_query_cache.cpp:1645-1648`) | Não | **Não** |
| P8 | Release de QMD (`add`) | `kepler_compute.cpp:169-174`: `Read` e `Write<u32>` diretos na memória do jogo, dentro de `SignalFenceCoalesced`; `MemoryManager::Write` é um `memcpy` sem aviso aos caches (`memory_manager.cpp:301-310`) | Não | **Não** |
| P9 | Cópia DMA que cai no caminho lento | `maxwell_dma.cpp:143-149,216-218,280-282,334-336`: lê com `SafeRead`, escreve na memória do jogo e acumula a invalidação (`memory_manager.cpp:492-495`), aplicada em `FlushCaching` (`:824-832`), que marca as páginas como modificadas pela CPU | Não | **Não** |
| P10 | `BufferClear` do DMA | `buffer_cache.h:283-304`: limpa no host, esquece o download; o motor também escreve a memória do jogo (`maxwell_dma.cpp:102-106`) | Não (subtrai de `gpu_modified_ranges`) | **Não** |
| P11 | Upload inline (Kepler memory, `AccelerateInlineToMemory`) | `vk_rasterizer.cpp:1220-1269`; `InlineMemory` só copia para o host se a região tem dados da GPU (`buffer_cache.h:2086-2128`) | Não (subtrai) | **Não** |
| P12 | Reset indireto feito na CPU | `vk_rasterizer.cpp:321-326`: escreve 20 bytes por registro por P11 | Não | **Não** |
| P13 | Decodificador de vídeo nativo | `native_video_decoder.cpp:487-488,568-569`: `InvalidateRegion` e `WriteBlock` no guest | Não (marca como modificado pela CPU) | **Não** |
| P14 | VIC (host1x) | `video_core/host1x/vic.cpp:918-919`: `gmmu_manager.WriteBlock`, que invalida (`core/device_memory_manager.inc:467-482`) | Não | **Não** |
| P15 | vi (buffers compartilhados) | `core/hle/service/vi/shared_buffer_manager.cpp:243-249`: escreve e chama `InvalidateRegion` | Não | **Não** |
| P16 | Escritas da CPU do jogo | `core/memory.cpp:703-767` → `OnCPUWrite` (`vk_rasterizer.cpp:994-1012`, `buffer_cache.h:160-180`): página com dados da GPU é **segurada**; senão `WriteMemory` marca a página (`:132-138`) | Não é produtor da GPU; invalida | Não se aplica, mas é o evento que torna a transferência inválida |

### 3.1 Pontos cegos do protocolo (produtores fora de `MarkWrittenBuffer`)

Lista separada, como pedido. Um filtro marcado só em `MarkWrittenBuffer` não vê:

1. **P6, imagens e render targets, e seus aliases.** Uma lista ou um argumento escrito por um shader numa
   imagem (ou numa imagem com alias de um buffer) não marca bit nenhum. Hipótese: raro para comandos; o único
   produtor de argumentos que o código descreve é o reset indireto do Bananza, que escreve registros por uma
   lista de ponteiros (`vk_rasterizer.cpp:255-258`), isto é, em buffers.
2. **P7 e P8, queries e semáforos.** O valor sai direto na memória do jogo. Em Fast, a fence roda o callback
   na hora (`fence_manager.h:81-96`), antes de a GPU emulada chegar ali; a escrita pode então aparecer
   **antes** do produtor que ela deveria ordenar. Isso é produtor de dado (valores de acquire), não de lista.
3. **P9 a P11, DMA lento, `BufferClear` e uploads inline.** Escrevem o guest e invalidam; um consumidor que
   lê o guest com `UnsafeRead` na hora os vê, mas fora do rastreio de produtor.
4. **P12, o reset indireto na CPU.** Substitui um produtor da GPU por uma escrita da CPU: o protocolo vê uma
   escrita de CPU onde o jogo esperava um dispatch.
5. **P13 a P15, HLE.** Escrevem com `InvalidateRegion`; o consumidor vê "modificado pela CPU", correto, mas sem
   ordem em relação ao fluxo da GPU (`gpu_thread.cpp:148-150` chama o rasterizador na thread de quem chama).
6. **P4 quando a origem não tinha dados da GPU** e **P1 além dos bytes realmente escritos:** o primeiro não marca
   o destino (a cópia no host e a atualização do guest em `buffer_cache.h:277-279` concordam, então não há
   versão errada, só ausência de marca); o segundo gera falsos positivos, não buracos.
7. **Memória global sem buffer correspondente** (P3): o recompilador não escreve nada, então não há produtor
   nem marca. Falha fechada pelo princípio 4, não ponto cego de ordem.

## 4. Os quatro mecanismos que os negativos do T1 provaram necessários

### 4.1 Acquire de syncpoint satisfeito pelo valor do host

**Hoje não existe espera.** `puller.cpp:128-132` só agenda `ReleaseFences`, e o comentário com `WaitFence` continua
desligado. O que existe são os dois contadores:

- `IncrementGuest` e `IncrementHost` em `host1x/syncpoint_manager.cpp:60-66`; `SignalSyncPoint` sobe o guest
  antes e o host depois, dentro da fence (`fence_manager.h:142-146`).
- A instrução de incremento do puller roda na trás (`puller.cpp:133-138`, `OnBack`), na ordem do fluxo.
- **Quando o host sobe (verificado, achado F1):** `SignalFence` põe o `func` (aqui `IncrementHost`) em
  `uncommitted_operations` e só o chama depois da fence **se** `delay_fence`; senão chama `func()` logo depois
  de `QueueFence` (`fence_manager.h:113,123-130`). `delay_fence` vale
  `IsGPULevelHigh()` com a opção de fence em Default, ou `Balanced`/`Accurate` (`:113`). Em Fast com Default,
  portanto, o host sobe no momento da sinalização, não da conclusão da GPU.
- Quem lê cada valor: o evento de espera do nvhost usa o **host** (`nvhost_ctrl.cpp:152,202`); o guest vê o
  valor **guest** pela memória (`memory_manager.cpp:281-285`), o decodificador (`native_video_decoder.cpp:127,603`)
  e as ações do composite (`gpu.cpp:332-335`).

Consequência (verificado, F1): "host ≥ v" garante, em Fast, que a trás **já gravou** tudo que veio antes do
incremento (a ordem é a do fluxo), e só nos modos adiados que a GPU **concluiu**. O protocolo que o plano propõe
precisa da primeira propriedade para o filtro e da segunda só para ler bytes: a cópia com fence própria cobre a
segunda. O harness do T1 modela o modo adiado (host na conclusão); ver F1 na seção 7.

### 4.2 Segurar escritas da CPU sobre entradas submetidas e não buscadas

**Não existe como tal.** O que existe, verificado:

- A CPU do jogo escreve em `HandleRasterizerWrite` (`core/memory.cpp:703-767`), que pergunta ao rasterizador
  (`OnCPUWrite`, `vk_rasterizer.cpp:994-1012`). `BufferCache::OnCPUWrite` devolve `true` (segurar) se o rastreio
  diz que a região tem dados da GPU (`buffer_cache.h:169-174`); senão grava `WriteMemory` (`:178`).
- Escrita segurada: `gpu_dirty_managers[core].Collect(address, size)` (`core/memory.cpp:765`), aplicada em
  `InvalidateGPUCache` (`gpu.cpp:123-163`): no início de cada lista (`gpu_thread.cpp:79,106`) e em cada fence
  (`fence_manager.h:139`). A aplicação junta faixas dentro de uma página (`gpu.cpp:138-161`,
  `EDEN_HELD_WRITE_MERGE`) e chama `OnCachedWrites` (`vk_rasterizer.cpp:958-992`), que usa `CachedWriteMemory`
  (`buffer_cache.h:141-157`).
- As entradas submetidas são protegidas **por outro meio**: o snapshot (`nvhost_gpu.cpp:392-406`). A segunda
  proteção, "não deixar a escrita da CPU chegar ao host antes de a entrada passar", só existe se o dado da
  entrada vem da GPU, e o consumidor que o leria não existe.

Diferença para o harness: no harness a escrita é segurada **enquanto houver entrada submetida e não buscada**
sobre aquelas faixas; aqui é segurada **enquanto a página tiver dados da GPU**, e liberada no início da próxima
lista ou na próxima fence, sem olhar para entradas. Uma escrita sobre memória de lista que o consumidor novo
ainda não buscou **seria aplicada** na próxima lista (hipótese, a validar com o caso `cpu_reuse` num consumidor
real).

### 4.3 Upload por palavra ao lado de dados da GPU (e a pendência da seção 11)

**Pergunta (plano de arquitetura, seção 11):** o upload de uma página marcada pela CPU sobrescreve bytes da GPU no
mesmo buffer? **Resposta: não, nos caminhos abaixo (verificado), com as ressalvas listadas.**

1. **Granularidade do rastreio.** O rastreio da CPU é por página de 4 KiB (`core/device_memory_manager.h:25-26`;
   `word_manager.h:24-26`): `ChangeRegionState` liga bits por página (`word_manager.h:118-144`) e
   `ForEachModifiedRange(Type::CPU, ...)` devolve faixas alinhadas à página (`:154-203`). Marcar uma escrita
   pequena marca a página inteira.
2. **O upload subtrai as faixas exatas da GPU.** `SynchronizeBuffer` pede as faixas de upload ao rastreio
   (`ForEachUploadRange`, `memory_tracker_base.h:140-145`) e, para cada uma, **pula** os intervalos de
   `gpu_modified_ranges` (`buffer_cache.h:1993-2000`), um `RangeSet` de intervalos exatos
   (`buffer_cache_base.h:539`) preenchido em `MarkWrittenBuffer` (`buffer_cache.h:1735-1736`). Os bytes da GPU
   ainda não baixados não são cobertos pela cópia da página.
3. **`WriteMemory` não baixa a página** e subtrai só a faixa escrita: `buffer_cache.h:132-138` faz
   `ClearDownload` e `gpu_modified_ranges.Subtract(device_addr, size)` apenas do intervalo escrito e depois
   marca a página como modificada pela CPU. O resto da página continua em `gpu_modified_ranges` e fica fora do
   upload pelo item 2.
4. **Escrita da CPU numa página com dados da GPU não passa por `WriteMemory`:** é segurada (seção 4.2) e
   aplicada por `CachedWriteMemory`, que, quando a região tem dados da GPU, grava **só os bytes escritos**
   (`InlineMemoryImplementation`, `buffer_cache.h:2105-2128`) depois de subtrair exatamente esse intervalo
   (`:2107-2108`). O comentário em `vk_rasterizer.cpp:1045-1047` descreve o motivo: marcar a página como
   modificada pela CPU "re-subiria inteira do guest, por cima do que a GPU escreveu e nunca foi copiado de
   volta".
5. **Os bytes da GPU só saem de `gpu_modified_ranges` depois de baixados** (`DownloadBufferMemory`,
   `buffer_cache.h:2136-2161`, que escreve no guest, `:2182,2189`) ou quando o jogo os sobrescreve
   (`WriteMemory`, `InlineMemoryImplementation`, `DMAClear`, `DiscardWrite`). O coletor de lixo baixa antes de
   apagar o buffer (`:76-77`); a junção de buffers copia no host (`:1886-1899`).

Ressalvas (hipóteses marcadas):

- **Android:** o ramo sem `use_optimized_vertex_buffers` (`buffer_cache.h:2008-2028`) **não** subtrai as faixas da
  GPU: lá o upload da página inteira sobrescreve (verificado no código; só roda com a opção desligada em
  Android, `:1971-1975`).
- **A proteção depende de `gpu_modified_ranges`.** Bytes escritos por produtores fora de `MarkWrittenBuffer`
  (seção 3.1) não estão lá. Para eles o upload da página é o que vale, e está certo quando o guest tem a versão
  boa (P9 a P11 e o DMA acelerado de origem sem dados da GPU escrevem o guest, `buffer_cache.h:277-279`), mas
  erraria para um produtor que escrevesse só no host e não marcasse. Não achei um produtor assim nos caminhos
  lidos; as imagens (P6) não escrevem em buffers sem passar por P1/P2/P5 (hipótese).
- **Máscara de "untracked":** `IsRegionModified(Type::GPU)` e `ForEachModifiedRange(Type::GPU)` ignoram páginas
  com o bit "untracked" ligado (`word_manager.h:170,216,231`), isto é, páginas já marcadas como modificadas pela
  CPU. Para essas páginas o rastreio da página responde "sem dados da GPU", e a proteção dos bytes passa só
  por `gpu_modified_ranges`. Não vi um caminho que perca bytes por isso, mas o par (bit de página, faixa exata)
  é uma duplicação a observar (hipótese).

Caso do Lichii (b13c4c1, #103): no fork de origem o `WriteMemory` baixava a página; aqui não baixa e não
precisa, pelos itens 2 e 3.

### 4.4 Validade das transferências no remapeamento

**Não existem transferências**, então nada tem validade. O que existe, verificado:

- O remapeamento do espaço da GPU chega a `ModifyGPUMemory` (`memory_manager.cpp:123-129,160-165`), que o
  rasterizador repassa ao texture cache (`vk_rasterizer.cpp:1085-1091`: imagens ganham `CpuModified` e
  `Remapped`, `texture_cache.h:702-720`) e ao buffer cache, onde só limpa `virtual_ranges`, e só se o runtime
  tem multi-range (`buffer_cache.h:124-129`).
- O estado do buffer cache é por endereço de dispositivo (`DAddr`): `gpu_modified_ranges`, o rastreio e os
  downloads não mudam com o remapeamento (verificado por ausência: `UnmapGPUMemory` não os toca).
- Desmapear memória de dispositivo (`UnmapMemory`, `vk_rasterizer.cpp:1073-1083`) chama `WriteMemory` no buffer
  cache e apaga as imagens.
- A tradução GPU→dispositivo é refeita em cada consulta (`GpuToCpuAddress` em `buffer_cache.h:220-221,285,310`); só
  consumidores que guardem endereços da GPU entre consultas (o `virtual_ranges`, o cache de transferências do
  protocolo novo) precisam da validade. O harness cobre isso (`remap`, `remap_ignore_valid_until`).

## 5. Efeitos do nível High (Accurate)

`Settings::UpdateGPUAccuracy` copia o nível para `current_gpu_accuracy` no **fim de cada lista**
(`common/settings.cpp:153-159`, chamado em `gpu.cpp:166-169`); `IsGPULevelHigh()` lê esse valor. Mapa completo
dos leitores de `IsGPULevelHigh` e `IsHighAccuracyPart` em `src/` (grep, fora da interface Qt/Android):

| # | Ponto | Código | O que muda em High (ou com a parte ligada) |
| --- | --- | --- | --- |
| E1 | Parte `MacroParameters` | `engines/maxwell_3d.cpp:267-268` | Relê parâmetros de macro (`ReadBlock` seguro). Em Switch 2, `NativeMacroRefresh` faz o mesmo em qualquer nível (`accuracy_parts.h:36-45`) |
| E2 | Parte `TextureFlush` | `renderer_vulkan/vk_rasterizer.cpp:871-880` | `MustFlushRegion` passa a incluir texturas. Afeta `IsMemoryDirty` (`memory_manager.cpp:518-540`) e, por ele, o `current_dirty` das macros, a escolha da busca de lista (`dma_pusher.cpp:148-149`), o acquire nativo (só buffers, `puller.cpp:36`) e o reset indireto (só buffers, `vk_rasterizer.cpp:298`). A CPU que lê uma textura modificada baixa em qualquer nível (`texture_cache.h:662-685`) |
| E3 | Parte `FlushOnInvalidate` | `gpu_thread.cpp:153-162`, `vk_rasterizer.cpp:1117-1123` | Baixa a região antes de invalidá-la numa escrita do guest que usa `FlushAndInvalidateRegion` |
| E4 | Parte `QueryOrdering` | `query_cache/query_cache.h:298-311` | Payload de semáforo/query vai pela `SyncOperation` na ordem da GPU; sem a parte, escreve na hora (`:300-309`) |
| E5 | Split desligado | `gpu_split.cpp:77-90` | Com High o split não pode rodar e uma thread só faz tudo; mudar de nível no meio do jogo para o split (`:82-86`) |
| E6 | DMA Safe por padrão | `dma_pusher.cpp:142-143`, `gpu_split.cpp:77-78` | Com `dma_accuracy = Default`, o modo seguro de leitura segue o nível High |
| E7 | Fences adiadas | `fence_manager.h:82,113` | Com a opção de fence em `Default`, High adia o callback da fence (host do syncpoint, valores de query) até a conclusão; senão roda na hora. Ver 4.1 |
| E8 | Query síncrona | `query_cache/query_cache.h:264` | `is_synced` só vale em Fast (com fence `Default`): em Fast a query de fence libera o slot logo depois de agendar (`:313-316`); em High fica registrada em `cached_queries` até a fence (`:317-330`) |
| E9 | Amostras precisas | `renderer_vulkan/vk_query_cache.cpp:177` | `VK_QUERY_CONTROL_PRECISE_BIT` na contagem de amostras |
| E10 | Renderização condicional | `renderer_vulkan/vk_query_cache.cpp:1520-1542` | Em High, compara no buffer (`HostConditionalRenderingCompareBCImpl`, `:1544-1548`); fora dele, o Intel proprietário do Windows (e Qualcomm, ARM e Turnip em qualquer nível, `:1522`) e o caso geral encerram a renderização condicional do host (`:1523,1540`); o efeito prático, desenhar sempre, é leitura minha do nome da função |
| E11 | Latência do nível | `gpu.cpp:166-169` | O nível só muda no fim da lista; uma lista em curso usa o valor antigo |
| E12 | OpenGL | `renderer_opengl/gl_rasterizer.cpp:503,632,685` | Mesmos efeitos de E2, E3 e E10 no backend OpenGL (fora de escopo; não abri os detalhes) |

Fora do nível, mas acoplados: a opção `gpu_fence_behavior` (`Balanced`/`Accurate`) liga E7 e E8 sem o nível e
controla `runtime.Finish()` no DMA (`buffer_cache.h:271-274`) e a sincronização em `FindBuffer`
(`buffer_cache.h:1750-1752`). `EDEN_GPU_HIGH_PARTS` liga E1 a E4 isoladamente (`accuracy_parts.h:13-34`) e
`EDEN_NX2_MACRO_REFRESH` liga E1 só para Switch 2 (`:36-45`).

**O que a lista da seção 3.5 do plano de arquitetura não tinha:** E8 (`query_cache.h:264`), E11 (latência do
nível) e os pontos OpenGL (E12). **O que ela tinha e continua igual:** as quatro partes, queries precisas,
renderização condicional, split, fences e DMA Safe.

## 6. Apresentação: o "frame à frente"

**O fenômeno (citado, não verificado aqui).** O plano (`docs/architecture-plan.md`, 2.2) cita o Eglantine
6a22450: o Bananza roda um frame à frente da thread da GPU e reescreve memória que ela ainda não leu. Não li
esse commit; o comentário do código daqui atribui a ideia do snapshot à PR #28 do mesmo fork
(`nvhost_gpu.cpp:392-394`). O que segue é o que **este fork** faz, verificado.

1. **Snapshot das palavras das listas.** `SubmitGPFIFOImpl` copia as palavras de cada entrada na submissão
   (`nvhost_gpu.cpp:392-407`) e o `DmaPusher` as usa (`dma_pusher.cpp:146-155`). Só em canais de ABI nativa
   (`:395`); só as palavras das listas.
2. **Conclusões sinalizadas antes da hora (Fast).** Com o comportamento padrão, o callback de fence roda na
   hora (`fence_manager.h:81-96,123-130`) e `IncrementGuest` roda antes do host (`:142-146`). O jogo vê o syncpoint
   subir e pode reaproveitar a memória enquanto a GPU emulada ainda não a leu. Com o nível High
   (ou `Balanced`/`Accurate`), o callback espera a fence e a janela fecha para os dados que dependem dele.
3. **Um composite em voo.** `HardwareComposer` espera o composite anterior antes de adquirir buffers
   (`nvnflinger/hardware_composer.cpp:78-79`; `gpu.cpp:340-350`). Os fences de aquisição do composite são
   registrados como ações **guest** (`gpu.cpp:332-335`), isto é, disparam no valor que sobe cedo.
4. **Escritas da CPU seguradas** sobre páginas com dados da GPU (seção 4.2): cobre a classe "o jogo reescreve
   dados que a GPU produziu e ainda não baixamos".
5. **Decodificador de vídeo.** O quadro é liberado quando o syncpoint **guest** chega ao valor da liberação do
   cliente: `WaitForReleaseFence` consulta `IsReadyGuest` (`native_video_decoder.cpp:650-668`) e, passados 100 ms,
   **escreve a imagem mesmo assim** (`:656-664`). O decodificador sobe guest e host do próprio syncpoint juntos
   (`:129-130,604-605`), porque nenhuma GPU participa dele. As correções anteriores desse caminho são
   1207e2b e d03d95d (`git log` do arquivo). A escrita do quadro usa `InvalidateRegion` seguido de `WriteBlock`
   (`:487-488,568-569`).
6. **Display de buffer nativo.** Sem imagem para o quadro, baixa antes do desswizzle da CPU
   (`vk_rasterizer.cpp:1280-1288`).

**O que este fork não faz (hipótese; não verificado em execução):**

- Não espera cada frame de um título (a solução por título do Eglantine, que a regra geral do plano rejeita).
- Não protege dados que não sejam palavras de lista: os uploads de buffer e de textura leem o guest **no
  momento do bind**, na trás (`buffer_cache.h:1969-2028`, `texture_cache.h:1181-1200`), e portanto podem ler
  bytes de um frame depois do que a GPU deveria ver, se o jogo já os reescreveu. Não existe contrapartida do
  snapshot para esses dados.
- A liberação para o jogo é pelo valor guest do syncpoint, que sobe antes da GPU emulada consumir os recursos
  (E7). Nenhum mecanismo faz o jogo esperar a conclusão real, exceto com fences adiadas.

Por isso a Apresentação é contrato próprio no plano: o snapshot é uma aplicação parcial da regra "nada que o
jogo vê como liberado pode ser reaproveitado antes de o emulador ler".

## 7. Achados que mudam os planos e perguntas abertas

Registrados aqui; os planos não foram alterados nesta tarefa, exceto a tabela da seção 3.5 do plano de
arquitetura (item 6 do pedido).

| # | Achado | Onde afeta |
| --- | --- | --- |
| F1 | O valor host do syncpoint, em Fast com fence `Default`, sobe na sinalização, não na conclusão da GPU (`fence_manager.h:123-130`); a frase "o valor que sobe na conclusão na GPU (`IncrementHost` na fence)" do plano de listas (4.2, item 2) só vale para fences adiadas. O harness do T1 modela o caso adiado. A espera real do acquire precisa dizer que garantia dá em cada modo: ordem do fluxo em Fast, conclusão no adiado | `docs/gpu-produced-data-plan.md` 4.2 item 2; `docs/architecture-plan.md` 2.2; harness (um modo "host cedo" é um caso a acrescentar) |
| F2 | O acquire de semáforo legado nunca espera (`continue` dentro de `do/while(false)`, `puller.cpp:187-270`); o nativo suspende sem limite (`:234-243`); o `acquire_timeout` é TODO (`:293`) | Seção 12.2 c do plano de arquitetura: o ponto de partida da espera com limite é mais fraco do que o texto sugere |
| F3 | Segurar escritas da CPU existe, mas condicionado a páginas com dados da GPU e liberado por lista/fence, não por entrada | 4.2; caso `cpu_reuse` do harness |
| F4 | O upload por palavra já está no caminho principal, com ressalva Android | 4.3; pendência da seção 11 respondida |
| F5 | Há dois rastreios que precisam concordar: bit de página (com máscara "untracked") e faixas exatas | 4.3 |
| F6 | Produtores de queries/semáforos escrevem o guest direto e, em Fast, cedo | 3.1, item 2 |

Perguntas abertas:

1. O consumidor novo precisa ler P7/P8 (valores de acquire escritos por queries) como dado produzido pela GPU?
   Se sim, `Query` precisa marcar o filtro e escrever na ordem do fluxo.
2. Imagens com alias de buffer (P6) produzem comandos em algum jogo? O Bananza usa buffers (hipótese).
3. A espera real do acquire de syncpoint (T4) deve usar o valor host ou o guest em Fast? (F1)

## 8. O que não foi aberto

- `shader_recompiler/` além do trecho de memória global; o interior de `vk_query_cache.cpp` fora das linhas
  citadas; `texture_cache` fora do que está citado; o código de `vic.cpp` além da escrita.
- Os forks (Eglantine, Lichii): nenhuma afirmação sobre eles foi relida; as citações vêm dos planos.
- O backend OpenGL: só os pontos de leitura de `IsGPULevelHigh`.
- Execução: nada foi rodado; os caminhos "só com X" são leitura de código.
