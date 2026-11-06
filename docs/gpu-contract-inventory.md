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

7. **Possível bug latente (seção 7.1):** uma escrita da CPU **segurada** pode ser desfeita pelo pop assíncrono
   de um download pendente antes de o aviso ser aplicado, e o upload então lê os bytes velhos. A sequência é
   possível pelo código lido (nenhuma trava a impede); nunca foi observada nem reproduzida. Grau: **possível
   bug latente, lido e não executado**. Uma segunda hipótese, de granularidade de 64 bytes, está na 7.2.

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
| C11 | Acquire de semáforo, canal legado | `puller.cpp:187-270`: dentro de `do { ... } while (false)`, cada `continue` (`:253,260,265`) salta para a condição `false` e **sai sem esperar** (leitura do código, não executado). `ProcessSemaphoreAcquire` (`:284-297`) espera sem limite (`acquire_timeout` é TODO, `:293`). **Decisão registrada (usuário):** é um padrão herdado do Eden/yuzu (o arquivo mantém o cabeçalho yuzu 2022, `puller.cpp:4-5`; a história git daqui não permite confirmar a origem do laço), **fora do escopo** porque mudá-lo afeta os jogos de Switch 1. Candidato a **contador no relatório de saúde** (`docs/architecture-plan.md`, 12.2 e), sem mudar o comportamento | Frente | Condição de sincronização | Sincronização |
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
| F1 | O valor host do syncpoint, em Fast com fence `Default`, sobe na sinalização, não na conclusão da GPU (`fence_manager.h:123-130`); a frase "o valor que sobe na conclusão na GPU (`IncrementHost` na fence)" do plano de listas (4.2, item 2) só vale para fences adiadas. **Corrigido** em `docs/gpu-produced-data-plan.md` 4.2 (itens 2 e 3) e na linha 9 da seção 11 dele; o harness do T1 passou a ter os dois modos (`host-early`, `host-deferred`) e os casos `syncpoint_before_gpu`, `syncpoint_trusted_as_ready` e `other_channel_missing_dependency` | Feito; a espera real do acquire (T4) precisa dizer que garantia dá em cada modo: ordem do fluxo em Fast, conclusão no adiado |
| F2 | O acquire de semáforo legado nunca espera (`continue` dentro de `do/while(false)`, `puller.cpp:187-270`); o nativo suspende sem limite (`:234-243`); o `acquire_timeout` é TODO (`:293`). **Decisão (usuário):** padrão herdado do Eden/yuzu; **fora do escopo** (mudar afeta os jogos de Switch 1); candidato a contador no relatório de saúde, sem mudar o comportamento | Seção 12.2 c do plano de arquitetura: o ponto de partida da espera com limite é mais fraco do que o texto sugere; a espera com limite vale para o caminho nativo e para o syncpoint, não para o legado |
| F3 | Segurar escritas da CPU existe, mas condicionado a páginas com dados da GPU e liberado por lista/fence, não por entrada | 4.2; caso `cpu_reuse` do harness |
| F4 | O upload por palavra já está no caminho principal, com ressalva Android | 4.3; pendência da seção 11 respondida |
| F5 | Há dois rastreios que precisam concordar: bit de página (com máscara "untracked") e faixas exatas | 4.3 |
| F6 | Produtores de queries/semáforos escrevem o guest direto e, em Fast, cedo | 3.1, item 2 |

Perguntas abertas:

1. O consumidor novo precisa ler P7/P8 (valores de acquire escritos por queries) como dado produzido pela GPU?
   Se sim, `Query` precisa marcar o filtro e escrever na ordem do fluxo.
2. Imagens com alias de buffer (P6) produzem comandos em algum jogo? O Bananza usa buffers (hipótese).
3. A espera real do acquire de syncpoint (T4) deve usar o valor host ou o guest em Fast? (F1)

### 7.1 Possível bug latente: escrita segurada contra download assíncrono pendente

**Sequência em questão:** a CPU escreve → o aviso aos caches fica segurado → o pop do download antigo grava a
memória do jogo por cima → o aviso é aplicado → o upload lê os bytes antigos.

**O que abri (cada item com `arquivo:linha`):**

| | Afirmação | Onde está |
| --- | --- | --- |
| a | A escrita da CPU vai para a memória do jogo **na hora** e o aviso aos caches fica **segurado**. `GetPointerImpl` chama `HandleRasterizerWrite` e só então devolve o ponteiro (`core/memory.cpp:587-590`), e o `memcpy` vem depois (`:649-653`). `HandleRasterizerWrite` pergunta ao rasterizador (`:751-755`) e, se ele segura (`true`), só registra o aviso em `gpu_dirty_managers[core].Collect` (`:765`, `core/gpu_dirty_memory_manager.h:26-48`). `OnCPUWrite` devolve `true` para página com dados da GPU **sem chamar `WriteMemory`** (`buffer_cache.h:169-174`). O aviso só chega aos caches em `InvalidateGPUCache` (`gpu.cpp:123-163`, `Gather` em `core/gpu_dirty_memory_manager.h:56-82`, aplicação em `vk_rasterizer.cpp:958-992`), chamado no início de cada lista (`gpu_thread.cpp:100-106`; no split, adiado para a trás, `:79`), no fim de cada `SignalFence` (`fence_manager.h:139`) e no `OnCPURead` (`gpu.cpp:266`). O próprio comentário diz "by default at the next fence" (`gpu_thread.cpp:101-102`) | verificado |
| b | No Vulkan a thread de fences publica downloads **de forma assíncrona**: `HAS_ASYNC_CHECK = true` (`vk_fence_manager.h:49`) cria `fence_thread` (`fence_manager.h:184-187`), que espera a fence do host e chama `PopAsyncFlushes` (`:241-263`), sob `buffer_cache.mutex` (`:286-293`). Sem a thread assíncrona o pop viria de `TryReleasePendingFences`, que só roda `if constexpr (!can_async_check)` (`:115-117,157-158`). O pop (`buffer_cache.h:703-730`) grava na memória do jogo, com `WriteBlockUnsafe`, **as partes da cópia que ainda estão em `async_downloads`** (`:719-722`) e depois as subtrai dali e de `gpu_modified_ranges` (`:723-725`). As faixas entram em `async_downloads` no commit da fence (`:682`, de `CommitAsyncFlushes`, `fence_manager.h:118`) | verificado |
| c | **Nada retira a faixa de `async_downloads` antes de a escrita segurada ser aplicada.** O único código que remove faixas é `ClearDownload` (`buffer_cache.h:210-216`), chamado em `WriteMemory` (`:134`), `DMACopy` (`:231`), `DMAClear` (`:295`), `ObtainCPUBuffer(DiscardWrite)` (`:337`), `InlineMemoryImplementation` (`:2107`) e `DownloadBufferMemory` (`:2159`); `async_downloads` só é tocado em `:211,682,719,723`. O ramo "segurar" de `OnCPUWrite` (`:169-174`) não chama nenhum deles | verificado |
| d | Quando o aviso é aplicado, `CachedWriteMemory` **lê a memória do jogo e sobe para o host**: com dados da GPU na região, `ReadBlockUnsafe` do guest (`buffer_cache.h:153-154`) e `InlineMemoryImplementation` (`:156`, `:2105-2128`: `ClearDownload`, subtração, upload dos bytes); sem dados da GPU (o pop já subtraiu a faixa, `:723-725`), `WriteMemory` (`:149`), que marca a página como modificada pela CPU, e o upload seguinte (`SynchronizeBuffer`, `:1993-2000`) copia o guest. Nos dois caminhos os bytes vêm do guest **depois do pop** | verificado |
| e | O que já protege algo parecido: `OnCPURead` aplica as escritas seguradas **antes** de baixar (`gpu.cpp:263-268`, comentário "from PokeDinner"). Isso cobre o download **síncrono** pedido pela leitura da CPU (`FlushRegion`, `vk_rasterizer.cpp:841-860`) e não o pop assíncrono da thread de fences. Os outros chamadores de `FlushRegion` (por exemplo `SafeRead`, `memory_manager.cpp:417,426`, e `ReadNativeSemaphoreWord`, `puller.cpp:39`) não aplicam as escritas seguradas antes: hipótese, não abri todos | verificado para o `OnCPURead`; hipótese para os outros |

**Conclusão: a sequência é possível neste fork**, por três seções críticas separadas, nenhuma cobrindo as
outras: a decisão de segurar (`buffer_cache.mutex`, `vk_rasterizer.cpp:997-1003`), o `memcpy` no guest (sem
trava) e o pop (`buffer_cache.mutex` na thread de fences) contra a aplicação (`buffer_cache.mutex` na
thread da GPU ou da trás, `vk_rasterizer.cpp:972-981`). **Condições para ela acontecer:**

1. A escrita cai numa página que o rastreio diz ter dados da GPU (`buffer_cache.h:169-174`), para ser segurada.
2. **Os mesmos bytes** estão num download pendente: a GPU os escreveu por `MarkWrittenBuffer`, que enfileirou
   o download (`:1735-1737`), e uma fence os commitou (`:682`) sem que o pop tenha rodado. Em canais de
   **Switch 2** (`UsesNativeTiling`) só entra no download o que está em `cpu_read_ranges`, isto é, o que a CPU
   já leu antes (`buffer_cache_base.h:546-562`, `NoteCpuRead`, `buffer_cache.h:733-743`); nos outros canais,
   toda faixa escrita pela GPU entra (`buffer_cache_base.h:554-558`), então a exposição é maior fora do Switch 2.
3. **Ordem:** o `memcpy` no guest vem **antes** do pop, e o pop vem **antes** da próxima `InvalidateGPUCache`.
   Se o pop vier antes do `memcpy`, nada se perde; se a aplicação vier antes do pop, o
   `ClearDownload` de `InlineMemoryImplementation` (`:2107`) tira a faixa de `async_downloads` e o pop a pula.
4. A interseção (escrita ∩ faixa do download) decide quais bytes se perdem: só ela.

**Efeito:** os bytes novos da CPU somem da memória do jogo e do buffer do host; o jogo lê o valor antigo.

**Grau de certeza:** a ausência de proteção é **verificada no código** (a, b, c, d); a **probabilidade** de a
janela abrir depende de tempo (latência da fence do host contra o intervalo até a próxima lista ou fence) e
**não foi medida nem reproduzida**; o modelo do harness reproduz a sequência quando o roteiro a força
(`held_write_vs_pending_download`). Registro como **possível bug latente**; nenhuma mudança de código foi feita.

**Direção, como pendência do T4 (sem correção agora):** (1) o pop respeita os avisos segurados ainda não
aplicados (o caso `held_write_vs_pending_download_protected` do harness); ou (2) as escritas seguradas são
aplicadas antes do pop. A declaração da seção 2.2 de cada uma está em `docs/gpu-produced-data-plan.md`, seção 12.

### 7.2 Segunda hipótese: avisos segurados em granularidade de 64 bytes

O `Gather` entrega as faixas em blocos de 64 bytes alinhados (`core/gpu_dirty_memory_manager.h:66-79`,
`align_bits = 6`, `:94-96`; `BuildTransform` arredonda para fora, `:113-121`), e a aplicação copia o trecho
arredondado do guest (`buffer_cache.h:153-156`, `InlineMemoryImplementation`, que também subtrai o trecho
inteiro de `gpu_modified_ranges`, `:2107-2108`). **Hipótese (lida, não executada):** bytes vizinhos da GPU,
dentro do mesmo bloco de 64 bytes que a escrita da CPU e ainda não baixados, seriam cobertos pelos bytes
antigos do guest. **Modelada no harness (T3, item 4):** `held_write_block_clobbers_gpu_bytes` reproduz a perda
com avisos em blocos de 64 bytes (falha com `WordMismatch` em todas as rodadas) e `held_write_block_exact_notices`
(avisos exatos) passa; **problema separado da janela do pop** (7.1), sem download no roteiro. No emulador
continua sendo caminho plausível por leitura, sem ocorrência nem frequência medidas; o build de diagnóstico
(`docs/list-diag-design.md`, 5) conta os bytes da GPU cobertos por bloco aplicado. Em Switch 2 só se baixa o que a
CPU leu (`buffer_cache_base.h:541-562`), então os vizinhos que a CPU nunca leu **só existem no host**: o upload do
bloco os troca pelos bytes antigos do guest e a subtração em `gpu_modified_ranges` impede que alguma coisa os
devolva.

### 7.3 Chamadores de `InvalidateGPUCache` e `Gather`: exclusão entre `Gather` do mesmo gerenciador

Pedido pela revisão do desenho do T3 (`docs/list-diag-design.md`, 3.3). **Fato do código (verificado):** o `guard`
do `GPUDirtyMemoryManager::Gather` protege só a troca dos buffers (`core/gpu_dirty_memory_manager.h:59-64`); a
iteração e o `clear()` do `front_buffer` ficam **fora** dele (`:66-81`). Dois `Gather` concorrentes no mesmo
gerenciador seriam uma corrida sobre o `front_buffer`, **preexistente** e independente do livro-razão do T3. O
único chamador de `System::GatherGPUDirtyMemory` é `gpu.cpp:134`, dentro de `InvalidateGPUCache`
(`grep` em `src/`); o único chamador de `OnCachedWrites` é `gpu.cpp:162`.

| # | Chamador | Thread com split | Thread sem split | Como confirmei |
| --- | --- | --- | --- | --- |
| 1 | `gpu_thread.cpp:79`, `Defer(InvalidateGPUCache)` no `SubmitList` | trás | (item 2) | lido |
| 2 | `gpu_thread.cpp:106` | n/a | thread única | lido |
| 3 | `gpu_thread.cpp:82`, `Defer(TickWork)` (a leitura do planejador dizia `:81`), que roda os pedidos síncronos (`gpu.cpp:197-208`), entre eles o do `OnCPURead` (`gpu.cpp:264-266`: `InvalidateGPUCache` e `FlushRegion`) | trás | `gpu_thread.cpp:109`, thread única | lido |
| 4 | `TickWork` no fim de `PrepareDraw` (`vk_rasterizer.cpp:348`) e de `DrawTexture` (`:505`) | thread que prepara os draws (trás) | única | lido o trecho; que a frente não chama o rasterizador sem `Drain` é do plano (`docs/architecture-plan.md`, 3.3), não reaberto |
| 5 | `fence_manager.h:139`, fim de `SignalFence`: roda na thread de **quem chama** `SignalFence`. Chamadores: `SignalSyncPoint` por `puller.cpp:133-138` (`OnBack`) e `maxwell_3d.cpp:682`; `SignalReference` por `puller.cpp:317` (`OnBack`); `WaitPendingFences` e `FlushCoalesced` por `ReleaseFences` via `OnBack` (`puller.cpp:131`), `OnCommandListEnd` (`dma_pusher.cpp:45-52`, `Defer`) e `OnGpuThreadIdle` (`gpu_thread.cpp:58`, `Defer`); `SignalFenceCoalesced` adiado (`kepler_compute.cpp:171`, `query_cache.h:296`) só com fences adiadas | trás | única | lido. `query_cache.h:586` (`RequestGuestHostSync` chama `ReleaseFences`): thread de quem chama o query cache; **não abri o chamador** |
| 6a | **Frente, depois de `Drain`:** `dma_pusher.cpp:176-183`, `Drain` e `rasterizer->SignalFence` (só com "Sync Memory Operations") | **frente** | n/a | lido (a leitura do planejador já a tinha) |
| 6b | **Frente, depois de `DrainBack`:** `puller.cpp:225` (`ReleaseFences(true)`), `:252`, `:259`, `:264` (`ReleaseFences()`, cujo padrão é `force = true`, `rasterizer_interface.h:104`), depois do `DrainBack` de `:186`; e `:291`, depois do de `:285`. `ReleaseFences` chama `WaitPendingFences(force)` (`vk_rasterizer.cpp:1113-1114`), que com `force` faz `SignalFence` (`fence_manager.h:148-172`) e portanto `InvalidateGPUCache` **na frente** | **frente** | n/a | lido. **Estes cinco não estavam na leitura do planejador** |
| 7 | `puller.cpp:339` (`MemOpB`, `OnBack`) e `maxwell_3d.cpp:435` (`invalidate_texture_data_cache`, método do engine) | trás | única | lido o ponto de chamada; que o método do engine roda na trás (inclusive com as macros na frente, que mandam as escritas por `MacroWrite`, `gpu_split.h`) é **hipótese** |
| 8 | **Thread de fences (`ReleaseThreadFunc`, `fence_manager.h:241-272`)** | **não chama** | n/a | lido: espera a fence, `PopAsyncFlushes` e executa as operações guardadas; nenhum chamador de `SignalFence` acima passa `InvalidateGPUCache` como operação (as que vi: `IncrementHost`, `[]{}`, a do bit de sync, a de espera de `WaitPendingFences`, escritas de query e de release de QMD) |
| 9 | Threads da CPU do jogo e de HLE | não chamam | n/a | lido: `OnCPURead` só enfileira (`RequestSyncOperation`, `TickGPU`, `WaitForSyncOperation`, `gpu.cpp:261-270`) |

**Por que a exclusão vale nos caminhos lidos, e por que só por convenção:**

- **Com split**, a trás só executa o ring, e as chamadas diretas da frente (6a, 6b) vêm depois de um `Drain`. O
  `Drain` volta quando `consumed == write_position` (`gpu_split.cpp:243-268`), e a trás grava `consumed` com
  `seq_cst` **depois** de executar as entradas (`StoreConsumed`, `:211-218`, chamado de `:430` e `:434`); a frente
  não envia nada entre o `Drain` e a chamada. O comentário de `puller.cpp:178-181` diz isso: "From here on the
  back is idle, and the front may use the rasterizer until it sends more". Logo, o `InvalidateGPUCache` da frente
  e o da trás **não se sobrepõem**. **Sem split**, é uma thread só.
- **Nada impõe isso por trava.** É uma convenção do `Drain`; o `Gather` não a verifica. O `static thread_local`
  de `gpu.cpp:129` e o comentário "Per thread: fences can also apply them" mostram que **mais de uma thread**
  chama, não que sejam concorrentes.
- **Não abri:** `GpuSplit::MaybeStart` e `Stop` (a transição do split no meio da sessão), o chamador de
  `query_cache.h:586`, quem chama `PrepareDraw` e `DrawTexture`, e o dispatch de `maxwell_3d.cpp:435`. Se algum
  deles violasse a exclusão, seria uma **possível falha preexistente** (corrida sobre o `front_buffer`), registrada
  aqui e separada do livro-razão.
- **Conclusão (leitura, não execução):** nenhuma violação encontrada nos caminhos lidos. Vira **precondição P0**
  do protocolo do T3 (`docs/list-diag-design.md`, 3.3: um `Gather` por vez por gerenciador, aplicações na ordem
  das retiradas). O diagnóstico **não** a corrige nem a esconde: conta `gather_overlapped` (`gather_active` não
  nulo na entrada) e `apply_out_of_order`, e trata como ambíguos os lotes até ali.

### 7.4 Bug preexistente: um `Collect` que cruza o limite de 2 KiB perde bytes (verificado em GCC)

Achado do desenho do T3a.1 (`docs/list-diag-design.md` 3.7.1) e confirmado pelo planejador com um programa pequeno
contra o `gpu_dirty_memory_manager.h` real (stub de página do harness, `g++ -std=c++20`).

- **Código (verificado).** A transformação cobre 2 KiB, com uma máscara `u32` de blocos de 64 B (`page_bits =
  DEVICE_PAGEBITS - 1`, `align_bits = 6`, `gpu_dirty_memory_manager.h:181-186`). `BuildTransform` calcula
  `top_bit = (minor + size + 63) >> 6` (`:204-212`), sem cortar no fim da transformação. Se a escrita passa de 2 KiB,
  `top_bit > 32` e `CreateMask` desloca por `32 - top_bit`, que dá a volta em `size_t` (`:195-202`): comportamento
  indefinido.
- **Medido (planejador, GCC 13, x86-64, `-O2`):**

  | `Collect` | O `Gather` devolve |
  | --- | --- |
  | `[0x10000,+4096)` | `[0x10000,+2048)`: **a segunda metade some** |
  | `[0x10400,+2048)` | **nada** |
  | `[0x107c0,+128)` | **nada** |
  | `[0x10000,+3000)` | `[0x10000,+960)` |
  | `[0x10000,+2048)` e `[0x10040,+64)` | certos |

  Com `-fsanitize=undefined`: `gpu_dirty_memory_manager.h:197: shift exponent 18446744073709551584 is too large for
  32-bit type 'unsigned int'`.
- **Quem chega lá (verificado).** `HandleRasterizerWrite` (`core/memory.cpp:704`) recebe pedaços de `WalkBlock`, de
  até uma página de 4 KiB: `WriteBlock` (`:322`, `:337`, escritas de HLE e kernel), `ZeroBlock` e
  `StoreDataCache`/`FlushDataCache` (`:384`, `:393`, a limpeza de cache que o jogo faz antes de entregar memória
  à GPU). Uma escrita `Write<T>` desalinhada que cruze `0x800` também chega (até 16 B, rara).
- **Efeito (hipótese).** A escrita segurada nunca chega ao `Gather`. A GPU continua com os bytes velhos daquela
  parte da página até outro aviso cobri-la: incoerência silenciosa, em qualquer jogo. **No MSVC** o resultado é
  outro comportamento indefinido; o deslocamento de hardware em x86 usa os 5 bits baixos e daria o mesmo padrão,
  mas isso não foi conferido no binário. Não se sabe com que frequência acontece no FE ou no Bananza.
- **Origem:** o gerenciador veio do yuzu; não foi introduzido por este fork (**hipótese**, histórico não aberto).
- **Interação com o diagnóstico:** o aviso é registrado com o tamanho inteiro, mas a aplicação só cobre parte. O
  registro fica pendente e expira, ou aparece como `apply_partial`. Parte de `notice_expired` pode vir daqui
  (**hipótese**).
- **O que é garantido:** o comportamento indefinido, não o resultado. A tabela acima é o que o GCC produziu e não
  vale como previsão para o MSVC (revisão).
- **Plano:** item D1 (`docs/handoff-planner.md`, 7.1): contar as ocorrências no `BackStats`; recortar dentro do
  `CollectT`, nas duas variantes, com a correção ligada por padrão; uma chave temporária só para o A/B.

### 7.4.1 D1: desenho da correção e da contagem (sem código)

**Estado: implementado em `dee6963`; A/B feito (resultado abaixo); chave removida em `7addd24`** (o revisor vê o diff antes de o usuário compilar). O texto
abaixo é o desenho; os três ajustes do revisor (marcados **AJUSTE**) já estão incorporados. Números de linha: os de `72e41f1`.

**Recorte, dentro de `CollectT` (`core/gpu_dirty_memory_manager.h:60-85`), nas duas variantes.** O corpo atual
(`:65-84`: `BuildTransform` e o laço de CAS) vira `CollectOne<Diag, Hooks>(address, size)`, sem mudança alguma (mesmas
ordens). `CollectT` passa a ser:

```
size == 0:  retorna SEMPRE, antes de tudo (sem ler a chave, sem máscara)                  // AJUSTE 1
size <= page_size - (address & page_mask):  CollectOne(address, size)   // o caminho de hoje, sem laço e sem ler a chave // AJUSTE 2
senão, chave desligada:  CollectOne(address, size)   // o código de hoje, com comportamento indefinido: só referência experimental
senão:  repete  n = min(size, page_size - (address & page_mask));  CollectOne(address, n);  address += n;  size -= n;
        entre dois pedaços: Hooks::At("c.between_pieces")
```

- 2048 é `page_size` (`:182`, `1 << page_bits`); a conta usa a constante, não o literal. Cada pedaço cabe numa
  transformação (`top_bit <= 32`), então `CreateMask` (`:195-202`) nunca recebe deslocamento negativo.
- A chave é lida **só** quando a faixa cruza 2 KiB: o caso comum (cabe) não paga nem uma carga. Com a chave em 0 o
  tamanho zero também segue o código de hoje (a saída idêntica do teste).
- As ordens de memória de cada pedaço são as da variante (`CollectOne` as calcula como hoje). Nenhuma trava nova: o
  laço chama `CollectOne` em sequência, cada chamada com o seu `guard`/CAS próprio.
- O hook de teste `Hooks::At` ganha um ponto `c.between_pieces` entre dois pedaços (vazio em produção), para o teste
  determinístico.

**A chave, lida uma vez, num header de `core/`.** Em `gpu_dirty_memory_manager.h`, antes da classe, no estilo de
`g_parts` (`video_core/list_diag.h`):

```
inline std::atomic<u8> g_dirty_split_2k{ParseSplitKey(std::getenv("EDEN_DIRTY_SPLIT_2K"))};  // 0 só se o texto for "0"
inline bool DirtySplit2K() { return g_dirty_split_2k.load(std::memory_order_relaxed) != 0; }
```

`ParseSplitKey` é `constexpr`-amigável: ausente ou qualquer valor diferente de `"0"` → 1. É inicializada na carga do
programa (uma vez), e a leitura é uma carga relaxada, só no caminho que cruza. **O teste a liga e desliga** com
`g_dirty_split_2k.store(1/0)` com o sistema parado (como os testes fazem com `g_parts`); o harness não precisa
de variável de ambiente. **Comentário no código e neste doc: a chave é TEMPORÁRIA, existe só para o A/B do usuário
e será apagada depois dele (o comportamento correto fica fixo); não é uma opção permanente.**

**Com `EDEN_LIST_DIAG` ligado.** `CollectObserved` (`protocol.h:99-`) chama `CollectT<true>` entre as leituras `s`
e `s2`; o laço fica **dentro** dessa chamada, então todos os pedaços ficam entre `s` e `s2`, com as mesmas ordens
`seq_cst`. A suficiência de `s == d && s2 == s` vale para **cada** pedaço pelo argumento da 3.7.3 (caminho efetivo
`A_p` de cada pedaço, antes de `s2` na ordem do programa): sem incremento de `lot_started` entre `s` e `s2`, nenhuma
retirada começou entre os dois, e todos os pedaços são do lote `d + 1`. Um `Gather` que comece **entre** os pedaços
incrementa `lot_started` antes de retirar e, como o incremento precede a retirada e ela precede o segundo pedaço
ou o segue, `s2 != s`: o aviso sai **ambíguo**, com `hi = s2 + 1` (3.7.3, passo 3). **Um aviso dividido entre
lotes nunca recebe um lote só**: só o caso inequívoco atribui `d + 1`. O registro é um só (o aviso inteiro, com o
tamanho original); o que a aplicação cobre de cada lote é decidido pelo `exact`/`Suspect` como hoje.

**Contagem (só com `EDEN_BACK_THREAD_STATS=1`).** Sem as estatísticas **nenhum contador é atualizado**. O que o fonte não
elimina: a checagem de `BackStats::Enabled()` (uma carga e um desvio) e a passagem de `Src` a `HandleRasterizerWrite`; o custo
disso depende do código gerado e **não foi medido nem conferido no binário**.

1. *Pedaços que cruzam 2 KiB.* Em `core/memory.cpp`, no lambda de `HandleRasterizerWrite`, **imediatamente antes** da
   chamada `CollectNotice` (`:768`), dentro de `if (BackStats::Enabled())`:
   `if (GPUDirtyMemoryManager::CrossesTransform(address, size)) {   // size > page_size - (address & page_mask), a mesma função de CollectT Add(shared.dirty_cross_calls, 1); Add(shared.dirty_cross_bytes, size);
   Add(shared.dirty_cross_by_source[src], 1); }`. Só passam por ali os pedaços **segurados** (o lambda retorna antes
   se `OnCPUWrite` não segura, `:752-755`); é o conjunto que o `Collect` vê. Também `dirty_held_calls` (o denominador).
   **Fonte:** `HandleRasterizerWrite(v_address, size, src = Src::Other)` ganha um parâmetro `enum class Src : u8`,
   constante em cada chamador (`WriteBlockImpl` `:322` = `WriteBlock`; `ZeroBlock` `:337`; `StoreDataCache` `:384` e
   `FlushDataCache` `:393` = `CacheOp`; `Write<T>` `:652`; `WriteExclusive*` `:665`, `:679` = `Exclusive`). O parâmetro
   só é lido dentro do `if (Enabled())`: o compilador o dobra, sem custo fora das estatísticas (a conferir no binário).
2. *`InvalidateGPUCache`.* Em `video_core/gpu.cpp`, no `Impl::InvalidateGPUCache`: `invalidate_total` na entrada
   (`:124`, ao lado do `ScopedSharedTime`), `invalidate_empty` no `cached_writes.empty()` (`:147`), e `invalidate_data`
   logo depois da junção, antes de `OnCachedWrites` (o contador `held_writes.batches` já existe, `vk_rasterizer.cpp:959`,
   e deve bater com `invalidate_data`: serve de conferência). Todos atrás de `BackStats::Enabled()`.
3. *Estrutura.* `struct Shared` (`back_thread_stats.h:168-240`) ganha: `dirty_held_calls`, `dirty_cross_calls`,
   `dirty_cross_bytes`, `dirty_cross_by_source[6]`, `invalidate_total`, `invalidate_empty`, `invalidate_data`, todos
   `std::atomic<u64>` com `Add` relaxado como os vizinhos. (Linha compartilhada entre os seis núcleos: aceitável porque
   só existe com as estatísticas ligadas, que já pagam isso nos `cpu_write_*`.)

**Linha do Report** (`back_thread_stats.cpp`, depois de "Guest CPU writes to cached pages", `:335`):

```
Dirty 2 KiB crossings (EDEN_DIRTY_SPLIT_2K={on|off}): held pieces {held:.0f}/s, crossing 2 KiB {n:.1f}/s ({b:.0f} B/s,
{pct:.2f}% of held); by source: Write<T> {a:.1f}/s, WriteBlock {b:.1f}/s, ZeroBlock {c:.1f}/s, cache op {d:.1f}/s,
exclusive {e:.1f}/s; InvalidateGPUCache {t:.0f}/s: empty {z:.0f}/s, with data {w:.0f}/s
```

`on|off` vem de `DirtySplit2K()`. O texto "temporary switch, removed after the A/B" entra no `LOG_INFO` quando a chave está
em `off`, para um log antigo não ser lido como configuração permanente.

**Testes** (`tools/list-diag-harness/`, header real, variantes `<false>` e `<true>`, builds `-O2`, TSan e
ASan+UBSan **com zero erro de UBSan**: o `shift exponent` do 7.4 não pode aparecer com a chave ligada):

| # | Teste | Confere |
| --- | --- | --- |
| 1 | Faixa exata de 2 KiB (`[0x10000,+2048)`, `[0x10800,+2048)`) | um pedaço; devolve a faixa inteira |
| 2 | Página inteira de 4 KiB (`[0x10000,+4096)`) | dois pedaços; `Gather` devolve `[0x10000,+4096)` (pode vir em duas faixas contíguas) |
| 3 | Início desalinhado (`[0x10033,+3000)`, `[0x107ff,+2)`) | cobre todo byte, arredondado a 64 B |
| 4 | Cruzamento curto `[0x107c0,+128)` | duas faixas de 64 B: `[0x107c0,+64)` e `[0x10800,+64)` |
| 5 | Tamanho zero não muda nada | **AJUSTE 3:** com a chave ligada e desligada, nas duas variantes, `Gather` vazio e sem UB |
| 6 | Os casos da tabela do 7.4 | `[0x10000,+4096)`, `[0x10400,+2048)`, `[0x107c0,+128)`, `[0x10000,+3000)`, `[0x10000,+2048)`+`[0x10040,+64)`: faixas inteiras, arredondadas a 64 B |
| 7 | Equivalência aleatória | para `(endereço, tamanho)` dentro de uma página de 4 KiB, a união do `Gather` cobre **todo byte escrito** e não passa do arredondamento de 64 B; nas duas variantes |
| 8 | Equivalência com o legado | **AJUSTE 3:** com a chave em 0, saída idêntica à do legado (`list_diag_legacy_manager.h`) **só** em entradas válidas, que não cruzam 2 KiB. A tabela do 7.4 com a chave em 0 **não é teste** (comportamento indefinido, sem saída garantida); a correção é validada contra a união esperada, arredondada a 64 B, com UBSan limpo |
| 9 | Determinístico: `Gather` entre os dois pedaços de um aviso, diagnóstico ligado | parada em `c.between_pieces`; o `Gather` retira; o aviso sai **ambíguo**, nenhum dos dois lotes o afirma (`blk_affirmed = 0`), os dois pedaços aparecem em lotes diferentes na saída do `Gather` |
| 10 | Pedaços sem `Gather` no meio | aviso inequívoco, os dois pedaços no lote `d + 1` |
| 11 | Chave lida uma vez | `DirtySplit2K()` só muda com `store` explícito do teste |

**Resultado dos harnesses (`dee6963`).** `tools/list-diag-harness/run.sh`, 32 casos de teste (8 novos, `[d1]`; os testes 5/8 já com o
AJUSTE 3): `-O2`, TSan e ASan+UBSan, cada um **All tests passed (77312 assertions in 32 test cases)**; o UBSan
(`-fno-sanitize-recover`) não acusa nada com a chave ligada. Sensibilidade: com o recorte forçado a desligado, 5 dos 8 casos
`[d1]` falham (7029 asserções). O harness dos contratos (`tools/gpu-contract-harness`) não inclui o gerenciador real e não foi
rodado de novo. **Não compilado aqui:** `core/memory.cpp` e `video_core/gpu.cpp` (faltam os cabeçalhos do ffmpeg); a
`back_thread_stats.cpp` só dá os erros de versão do `fmt` que o arquivo sem a mudança já dá.

**Conferência do planejador (`de5bc3d`).** Os três builds do harness foram reproduzidos na nuvem (GCC 13): os mesmos
77 312 asserções em 32 casos, sem falha nem erro de UBSan. A sensibilidade também: forçando o recorte a desligado
(edição temporária, revertida), 5 de 8 casos `[d1]` e 7 029 asserções falham. O diff foi lido.
`memory.cpp` e `gpu.cpp` continuam sem compilação na nuvem; o build do usuário no MSVC é o primeiro.

**Medição do A/B (decisão do planejador).** O custo é medido em **draws/s das estatísticas, ligadas nos dois braços**
(`EDEN_BACK_THREAD_STATS=1`), e não pelo FPS da barra do Eden. Motivos:
- É a única métrica que já foi validada: ruído de ~1% entre as rodadas iguais a e c (`docs/list-diag-design.md` 8.4).
- O FPS anda em degraus (30/24/22/20) e não resolve diferenças de 5%, que é o orçamento.
- O código das estatísticas, inclusive a contagem nova do D1, é o mesmo nos dois braços, e a mesma rodada dá a
  contagem dos cruzamentos. **Interpretação (correção do revisor):** o A/B mede o efeito de habilitar o D1 **nesta
  configuração instrumentada**, incluindo o trabalho de invalidação recuperado e as interações com a instrumentação. A
  frequência, a contenção e a divisão entre chamadas vazias e com dados podem mudar entre os braços, então o custo da
  instrumentação não se cancela necessariamente.

O que isso não mede: o custo absoluto e o efeito relativo sem as estatísticas. Isso fica declarado como limitação; não é preciso para
decidir o D1. Se a diferença ficar acima do ruído, o `other busy` e a linha `Invalidation batches` dizem se o custo
novo é aplicação de escritas que antes se perdiam.

Protocolo: FE no mesmo lugar das rodadas a–d (25 s depois de carregar o save, parado), quatro rodadas alternadas,
com `EDEN_DIRTY_SPLIT_2K=0` e sem ele, sem `EDEN_LIST_DIAG`. Bananza: duas rodadas, uma de cada, no mesmo ponto
depois de carregar o save.

**Resultado do A/B (medido pelo usuário, 2026-11-06; logs `2a` a `2d` do FE e `banana2a`/`banana2b`).** Build com o
D1, conferido pela linha `Dirty 2 KiB crossings` em todos os logs. A string de versão continua velha, como antes. A
chave em cada log foi lida da própria linha (`on` em 2a, 2c e banana2a; `off` em 2b, 2d e banana2b). Lugar e
configuração do protocolo acima: o FE parado no ponto de sempre, só `EDEN_BACK_THREAD_STATS=1`, sem diagnóstico.

| Rodada FE | Chave | draws/s (média, amostras com a trás ≥ 85%) | `other busy` |
| --- | --- | --- | --- |
| 2a | on | 78 996 (5, 40-56 s) | 15,2% |
| 2b | off | 79 412 (5, 41-57 s) | 15,2% |
| 2c | on | 78 981 (5, 42-58 s) | 15,1% |
| 2d | off | 78 612 (4, 43-55 s) | 15,2% |

- **Custo:** ligada 78 989, desligada 79 012: **−0,03%**. Nas repetições novas o ruído entre rodadas iguais foi de 0,02%
  (2a/2c) e de 1,0% (2b/2d), o que confirma a referência de ~1%. Não há custo mensurável nesta configuração.
- **Leitura dos contadores:** `dirty_cross_bytes` soma o tamanho **inteiro** dos avisos que cruzam, não só os bytes além
  da fronteira nem os que se perdiam. Os contadores do `InvalidateGPUCache` são lidos um por um no relatório, então
  `total = vazias + com dados` pode diferir um pouco numa janela, por chamadas que atravessam o instante da leitura; isso
  não é perda de chamadas.
- **Cruzamentos de 2 KiB segurados: zero** em todas as amostras dos seis logs, nos dois jogos, com ~1 milhão de
  pedaços segurados por segundo no FE e até ~2 milhões no Bananza. Nessas cenas, o bug não acontece. As escritas
  grandes (`WriteBlock`) em páginas da GPU devem ser escritas na hora pelo cache (`written`), e não seguradas
  (**hipótese**; o contador só vê as seguradas). O contador é confiável: o `with data` do `InvalidateGPUCache` é igual,
  amostra por amostra, aos lotes de `held_writes` que já existiam (`vk_rasterizer.cpp:959`), e `CrossesTransform` é a
  mesma função que o harness testa.
- **Bananza:** as duas rodadas não chegam a um trecho estável (o log acaba em ~52 s, ainda subindo), então não há
  custo comparável.
- **Taxa de `InvalidateGPUCache` sem diagnóstico (nova):** no FE estável, ~1 800 chamadas/s, ~77% vazias, ~400/s com
  dados. Com o diagnóstico (8.4 do `list-diag-design.md`), os totais da sessão davam ~1 240/s e 48% vazias, com os lotes
  de escritas em ~1 040/s. Os números não são do mesmo trecho, então só sugerem que o diagnóstico faz **menos**
  chamadas e **mais** chamadas com dados, a hipótese 2 da 3.7.8 (**hipótese**). A rodada do T3a.1 mede as duas coisas
  no mesmo log.

**Decisão:** a correção fica, ligada sempre: ela remove um comportamento indefinido, sem custo medido. A chave
`EDEN_DIRTY_SPLIT_2K` sai, como prometido. A contagem continua só nas estatísticas, para que outros jogos e cenas
possam mostrar cruzamentos. O D1 não explica nada medido até agora no FE ou no Bananza.

**Interação com o T3a.1 e o resto.** O D1 vem antes do T3a.1 (o bug contamina o que ele mede). A aplicação de um
aviso recortado cobre as duas metades, e o `apply_partial` que o bug produzia deve cair (hipótese). Uma queda de
desempenho no A/B seria custo de invalidações antes perdidas: investigar, não restaurar a perda.

### 7.5 Possível bug preexistente: o contador de oclusão é zerado a cada submissão

- **Código (verificado).** `Scheduler::SubmitExecution` chama `EndPendingOperations` (`vk_scheduler.cpp:383-384`), que
  faz `query_cache->CounterReset(QueryType::ZPassPixelCount64)` (`:447-449`). Então qualquer submissão (`Flush`,
  `Finish`, as esperas que os downloads fazem) zera o contador de amostras que passaram no teste de profundidade, no
  meio de uma query que o jogo abriu.
- **Referência externa (relatada, não lida no código):** o Lichii do Frizti (release `frizti-v1`, zortos293#121 de
  Kajcsi, commits `e44ee5c` e `87203d5`) troca o zeramento por uma **pausa** e diz que isso acabou com flashes brancos de um
  frame no Ocarina of Time.
- **Efeito aqui (hipótese):** o resultado de uma query de oclusão depende de quantas submissões caíram dentro dela. Isso
  pode mudar a imagem ou a lógica do jogo de forma intermitente. Não foi medido no FE nem no Bananza.
- **Contrato:** é um contrato de query (`architecture-plan.md` 2.2): o valor que a GPU reportaria não pode depender
  de quando o emulador decide submeter. Candidato a item próprio, lido no código do Lichii e portado com A/B.
- **Para o diagnóstico:** o `blkcmp` faz um `Finish` por comparação, então numa rodada com ele há zeramentos a mais
  (`docs/list-diag-design.md` 3.8).
- **Precisão (revisão 7, verificado):** `ResetCounter` (`vk_query_cache.cpp:195-209`) pausa a query, abandona a atual e
  agenda o zeramento dos acumuladores. Toda submissão passa pelo reset; uma janela de contagem que atravesse a
  submissão pode perder acumulação. Isso não quer dizer que toda submissão interrompa uma query relevante.
- **O PR #121 do Lichii, pela leitura do revisor (a conferir pelo planejador quando o item começar):** são duas mudanças.
  (1) `CounterReset` vira `CounterClose` na submissão, preservando a contagem. (2) Um limite em `CloseCounter`: acima de
  `SamplesQueryBank::BANK_SIZE` slots, ele chama `ResetCounter` de novo. A (2) volta a descartar contagem por limite de
  recurso e não satisfaz o contrato numa query legítima longa. **Não portar o PR inteiro:** separar a preservação da
  contagem do controle de memória.
- **Testes do item:** reset → draws → submissão no meio → mais draws → report; o mesmo trabalho dividido em
  submissões diferentes dá o mesmo valor; query longa atravessando vários bancos; reset explícito do guest e retomada
  depois de uma pausa. O A/B visual e de desempenho complementa esses testes e não substitui a conferência do valor
  reportado.
- **Ordem:** item próprio, depois do resultado do T3a.2.

### Pendências de localização

**P1. Onde este fork produz os timestamps e as durações de GPU e de frame que o jogo lê.** Só localizado, com
`arquivo:linha`; não analisei qual deles o Ocarina usa nem como o controle de resolução dinâmica os consome
(pergunta 6 da seção 9 do plano de arquitetura).

| Tempo | Onde é produzido |
| --- | --- |
| Tick da GPU | `GPU::GetTicks` (`video_core/gpu.cpp:210-213`): `CoreTiming::GetGPUTicks` (`core/core_timing.cpp:240-244`, relógio de parede no multi-core, ticks da CPU convertidos no single-core) dividido pelo multiplicador do clock da GPU (`gpu.cpp:48`, configuração `gpu_clock`) |
| Timestamp escrito nos relatórios de semáforo e de query | `Maxwell3D::StampQueryResult` (`video_core/engines/maxwell_3d.cpp:557-561`); no cache de queries, na fence ou na hora (`video_core/query_cache/query_cache.h:283-286,300-304`); OpenGL `gl_rasterizer.cpp:441,453`; backend nulo `renderer_null/null_rasterizer.cpp:35` |
| Tempo de GPU lido por ioctl | `nvhost_ctrl_gpu::GetGpuTime` (`core/hle/service/nvdrv/devices/nvhost_ctrl_gpu.cpp:421-424`): devolve o tempo global do `CoreTiming` em nanossegundos, não os ticks da GPU |
| Vsync (evento que o jogo espera) | `Conductor::ProcessVsync` e `VsyncThread` (`core/hle/service/vi/conductor.cpp:76,86`), `VsyncManager::SignalVsync` (`vi/vsync_manager.cpp:15`), `GetDisplayVsyncEvent` (`vi/application_display_service.h:64`) |
| Ritmo e duração do frame na apresentação | `nvdisp_disp0::Composite` (`nvdrv/devices/nvdisp_disp0.cpp:101-105`): `ReportFrameInterval`, `SpeedLimiter::DoSpeedLimiting`, `PerfStats::EndSystemFrame`/`BeginSystemFrame`; `core/perf_stats.cpp:131-138` |
| Fatias de tempo de GPU do applet | `IWindowController::SetAppletGpuTimeSlice` é um stub (`am/service/window_controller.cpp:79-82`); `SetGpuTimeSliceBoost` em `am/service/applet_common_functions.cpp:87` (corpo não aberto) |

## 8. O que não foi aberto

- `shader_recompiler/` além do trecho de memória global; o interior de `vk_query_cache.cpp` fora das linhas
  citadas; `texture_cache` fora do que está citado; o código de `vic.cpp` além da escrita.
- Os forks (Eglantine, Lichii): nenhuma afirmação sobre eles foi relida; as citações vêm dos planos.
- O backend OpenGL: só os pontos de leitura de `IsGPULevelHigh`.
- Execução: nada foi rodado; os caminhos "só com X" são leitura de código.
