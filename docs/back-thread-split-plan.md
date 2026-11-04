# Plano: tirar trabalho da thread de trás da GPU (Fortune's Weave)

Escrito em 2026-11-04 sobre o `main` em `43a7f26`, a partir do pré-plano
(`docs/back-thread-split-preplan.md`). Para revisão antes de implementar. Foco: tempo, sincronização e locks.

## 0. Resumo

1. **Fase 0 (medir)** fica como no pré-plano, com mais contadores: tempo do worker do Vulkan (ocupado e
   ocioso), atraso do worker em draws e em frames, espera e posse dos locks dos dois lados, e o caminho de
   descritores de cada draw (descriptor buffer, push ou set), com a taxa de reaproveitamento e os bytes
   copiados e comparados por draw. Ela decide o resto.
2. **A opção A como está no pré-plano (uma thread nova dona do scheduler) não é viável.** Há ~220 chamadas ao
   scheduler fora do draw (caches, query cache, blits, compute pass, staging, present), várias com retorno
   usado na hora (`Finish`, `Wait`, `CurrentTick`, `UpdateGraphicsPipeline`, `IsRenderPassActive`).
   Detalhes na seção 1.
3. **Proposta para a fase 1:** o mesmo trabalho que A queria mover, sem mudar o dono do scheduler:
   - **1a, eliminar em vez de mover:** comparar o payload de descritores por identidade (ids de conteúdo)
     em vez de `memcpy` + `memcmp` de ~75 KB por draw com banco de texturas. Sem thread nova.
   - **1b, emissão no worker:** a escrita do descriptor buffer (`WriteDescriptorBuffer`, um
     `vkGetDescriptorEXT` por entrada) vai para dentro do comando gravado, e quem a executa é o
     `VulkanWorker`, que já consome os comandos em ordem. A ordem com fences, queries, `Drain`, present e
     `TickFrame` não muda, porque tudo continua no mesmo fluxo de chunks.
   - **1c (só se o worker saturar):** um estágio de preparo entre a thread de trás e o worker, no mesmo fluxo
     de chunks (3 estágios), em vez de uma fila paralela.
4. **Fase 2 (B)** só com os números da fase 1. O ganho grande (×1,3-1,5) exige mover o rasterizador inteiro
   ou dividir o `PrepareDraw`. As duas formas e as armadilhas já vistas no código estão na seção 9.

Expectativa honesta: a fase 1 deve mover ou eliminar ~5-12% da thread (×1,05-1,14), e só nos draws com banco de
texturas no caminho de descriptor buffer. É a parte segura. O salto para 30 FPS na praça depende da fase 2.

## 1. O que a leitura do código muda no pré-plano

| Fato | Onde | Consequência |
| --- | --- | --- |
| ~220 chamadas de produtor ao scheduler fora de `ConfigureDraw`: `vk_rasterizer.cpp` 57, `vk_query_cache.cpp` 30, `vk_texture_cache.cpp` 30, `vk_buffer_cache.cpp` 26, `blit_image.cpp` 18, `vk_compute_pass.cpp` 15, `renderer_vulkan.cpp` 11, `vk_swapchain.cpp` 8 etc. | grep de `scheduler.(Record\|Request\|Flush\|Finish\|Wait\|Update\|CurrentTick)` | "`scheduler.Record` só na thread de emissão" (fase 1 do pré-plano) obriga a rotear todas elas. As que precisam de retorno na hora teriam de esvaziar a thread de emissão antes, o que serializa tudo de novo. |
| `DescriptorBufferRing::Allocate` chama `scheduler.Wait`, `scheduler.Finish` e `CurrentTick` | `vk_descriptor_buffer.cpp:95-130` | A alocação fica com quem é dono do scheduler (a thread de trás). Só a **escrita** do conteúdo pode sair. |
| `UpdateDescriptorQueue::Acquire` faz `WaitWorker` no estouro e, ao crescer, **realoca** o vetor `payload` | `vk_update_descriptor.cpp:42-68` | Hoje é seguro porque o único leitor do payload é o worker e ele já foi esperado. Uma thread nova lendo o payload teria de ser esperada também, em todo `WaitWorker`. |
| O caminho de descriptor set (`UpdateDescriptorSet`) e o de push descriptor já rodam **dentro do lambda**, no worker. Só o caminho de descriptor buffer escreve na thread de trás. | `vk_graphics_pipeline.cpp:1012-1029` e `:1065-1114` | Com descriptor buffer (provável na RTX para os pipelines grandes: >32 descritores excluem o push descriptor), a escrita é a parte movível que sobra. A fase 0 confirma qual caminho é usado. |
| No acerto do cache de descritores da tabela compacta, `AddEntries` copia `desc.count` entradas de 24 B; depois `ConfigureDraw` compara o payload inteiro com `last_descriptor_payload` (`memcmp`) e, na falha, copia de novo (`assign`) | `pipeline_helper.h:483`, `vk_graphics_pipeline.cpp:1004-1028` | Com ~3100 entradas por draw (`reserve=3112` no log antigo): ~75 KB copiados + ~75 KB comparados por draw com banco, mesmo quando nada mudou. Isso pode ser eliminado (1a). |
| `DelayedDestructionRing` conta **frames** (`TickFrame`), não ticks da GPU: views e imagens apagadas morrem 8 `TickFrame` depois | `texture_cache_base.h:546-552`, `delayed_destruction_ring.h` | Um handle lido mais tarde por outra thread só é seguro se essa thread nunca ficar mais de ~7 frames atrás. Hoje o atraso do worker é limitado pelo present (`GetRenderFrame` espera um frame livre, `vk_present_manager.cpp:172-186`). 1b não aumenta esse atraso. Uma fila nova com profundidade própria aumentaria. |
| `DispatchWork` a cada 32 draws, `Flush` a cada 4096 draws ou 192 operações/2,5 ms | `vk_rasterizer.cpp:1013-1023`, `:1124-1144` | É a cadência de entrega ao worker. O trabalho movido para o worker chega em lotes de até 32 draws. |
| Os dois mutexes dos caches são `std::recursive_mutex`; o query cache pega `buffer_cache.mutex` por dentro | `texture_cache_base.h:297`, `buffer_cache_base.h:365`, `vk_query_cache.cpp:1410` etc. | Estreitar o lock de `PrepareDraw` (item L) exige conferir cada chamada que hoje está dentro dele. |
| `Finish` por frame no `DownloadMemory` da tabela de handles | `vk_graphics_pipeline.cpp:~501`, perf-handoff | É um ponto serial: a thread de trás espera o worker e a GPU. Tudo que for para o worker volta para o caminho crítico nesse ponto, na medida da fila do worker. Ver H8. |

Por que não "thread de emissão dona do scheduler" com um proxy (`Record` na thread de trás vira um item de fila
que a thread de emissão passa para o chunk): isso é exatamente o que o chunk já faz, com um salto a mais. O único
trabalho que essa thread poderia fazer de fato é o que o lambda já pode fazer no worker. Se o worker não tiver
folga, a resposta é 1c (um estágio no mesmo fluxo), não uma fila paralela com protocolo próprio de ordem.

## 2. Arquitetura proposta para a fase 1

```
front ──ring gpu_split──▶ back (dono dos caches e do scheduler) ──chunks (fila atual)──▶ VulkanWorker ──▶ vkQueueSubmit
                           resolve ids, handles, alocação                                 executa lambdas:
                           decide reuso e bind                                            escreve descriptor buffer (1b)
                           grava lambdas                                                  vkCmd*
```

Regras que valem para toda a fase 1:
- **R1. O scheduler continua com um único produtor: a thread de trás.** Nenhuma chamada nova de
  `Scheduler::*` fora dela. (Verificação em modo debug: a thread do primeiro `Record` fica guardada e as
  seguintes são conferidas; ver seção 8.)
- **R2. O worker não lê caches nem pega locks de cache.** O lambda só recebe dados prontos: ponteiros para
  memória com tempo de vida garantido (seção 7, H2) e valores capturados.
- **R3. Toda decisão que afeta a submissão ou o draw é tomada na thread de trás:** pular o draw, falha de
  alocação, reaproveitamento da alocação, bind do chunk do descriptor buffer, pipeline e rescale. O worker
  só executa.
- **R4. Interruptor fixo por processo** (lido uma vez, em `static const`): `EDEN_DRAW_EMIT=0` volta ao caminho
  atual; `=1` só 1a; `=2` 1a+1b (padrão depois de validado). Sem troca no meio da sessão, porque o estado de
  reaproveitamento por pipeline (`last_descriptor_payload` e a chave nova) depende do modo.

## 3. Fase 0: medir (um build, sem mudança de comportamento)

### 3.1 Como

- Interruptor próprio, `EDEN_BACK_THREAD_STATS=1`, separado do `EDEN_TEXTURE_TABLE_STATS`. Os
  temporizadores custam ~1-2% da thread, e a métrica de comparação (draws/s da linha `Texture tables`) deve
  ser lida com eles desligados. Desligado, custa um `if` com `static const bool` por ponto.
- Relógio: `__rdtsc()` (MSVC `<intrin.h>`, GCC `<x86intrin.h>`); o Zen 3 tem TSC invariante. Calibrar contra
  `steady_clock` no relatório (ticks/µs medidos entre dois relatórios). Fora de x86, `steady_clock`.
- **Carimbos encadeados**, um por fronteira: `t = Mark(bucket, t)` soma `now - t` no balde e devolve `now`.
  Cada trecho cai em um balde só, então os baldes somam o total, e "outros" = ocupado − soma. Nada de RAII
  aninhado (contaria duas vezes e esbarra no `/we4456` com nomes repetidos).
- Acumuladores `thread_local` (ou membros não atômicos) na thread de trás e no worker. As threads da CPU do
  jogo usam `std::atomic<u64>` com `memory_order_relaxed`, só com o interruptor ligado.
- Relatório a cada 4 s, emitido pela thread de trás, como a linha `Texture tables`. Os valores do worker são
  lidos com `relaxed` (é estatística, um relatório ligeiramente defasado não importa).

### 3.2 Baldes da thread de trás (µs/s e % do tempo ocupado)

1. `BackLoop`: ocupado (dentro de `Execute`) e ocioso (espera e sono): dá o total real e mostra se a thread
   é mesmo 100%.
2. Fora de draws: `CallMethod` e `CallMultiMethod` que não chegam a `ProcessDraw`, com uploads inline e DMA à
   parte (`AccelerateDMA::*`, `AccelerateInlineToMemory`).
3. `PrepareDraw`: `FlushWork` + `FlushCaching`; `CurrentGraphicsPipeline`; **espera pelo lock**
   (antes e depois do `scoped_lock`); dentro do lock: `ConfigureImpl` dividido em
   (a) VPRS e leitura de handles e `Build` (até `FillImageViews`); (b) resolução de views e samplers e bancos;
   (c) `bind_stage_info` e texture buffers; (d) `UpdateGraphicsBuffers` + `BindHostGeometryBuffers`;
   (e) por estágio, `BindHostStageBuffers` e `PushImageDescriptors` separados, e dentro do segundo os contadores
   de acerto, patch e falha de bloco compacto e do caminho elemento a elemento; (f) `PostCopyBarrier`,
   `UpdateRenderTargets`, `CheckFeedbackLoop`; (g) `ConfigureDraw` dividido em comparação do payload,
   `Allocate`, `WriteDescriptorBuffer`, `assign` e `Record`. Depois: `UpdateDynamicStates`, queries e
   transform feedback, `draw_func`, `FlushStorageWrites`. Por último, **posse do lock** (da aquisição à
   liberação).
4. `TickWork` no `SCOPE_EXIT` e `RendererVulkan::Composite` (present), à parte.
5. Esperas no scheduler vistas pela thread de trás: `WaitWorker`, `Finish` (com quem chamou:
   `DownloadMemory`, ring de descritores esgotado, texture cache) e `Wait(tick)`.

### 3.3 Contadores de descritores (por segundo)

- Draws por caminho: descriptor buffer, push, set, sem layout. Para descriptor buffer: alocação
  reaproveitada contra nova, entradas por draw, chamadas a `vkGetDescriptorEXT`, "frame exhausted"
  (`Finish` no ring) e tempo da espera de `frame_reused`.
- Bytes de `AddEntries`, bytes de `memcmp` em `ConfigureDraw`, bytes de `assign`.
- Estouros e crescimentos da fila de payload (já logados; aqui em contagem por segundo).

### 3.4 Worker, locks e frente

- Worker: tempo em `ExecuteAll` contra espera; chunks por segundo; profundidade da fila no `DispatchWork`
  (`work_queue.size()` já está sob `queue_mutex`).
- **Atraso do worker em frames:** `CommandChunk` ganha (só com estatística ligada) o número do frame
  (`TickFrame`) da thread de trás quando foi despachado. O worker registra o máximo de
  `frame_atual_de_trás − frame_do_chunk`. É o número que sustenta a segurança de H2 e deve ficar ≤ 2.
- Atraso da submissão: do `SubmitExecution` gravado ao `vkQueueSubmit` executado (o lambda de submissão
  carimba).
- Threads da CPU do jogo: contagem e tempo de espera nos locks de `OnCPUWrite`, `OnCachedWrites`,
  `OnCacheInvalidation`, `InvalidateRegion` e `FlushRegion` (relaxed). Também quantas vezes a thread de
  trás encontrou o lock ocupado (`try_lock` só para contar; se falhar, segue o `scoped_lock` normal).
- Frente: tempo em `Drain` e `WaitForSpace`, para confirmar que a frente nunca é o limite.

### 3.5 Saída e decisão

O usuário roda a praça (pior ponto, depois de estabilizar ~30 s), o palácio e uma batalha, e manda o log. A
sessão monta a tabela do pré-plano com números reais e decide pela tabela abaixo (limiares em % do tempo
ocupado da thread de trás):

| Condição medida | Ação |
| --- | --- |
| cópia + comparação de payload em acertos ≥ 3% | 1a |
| `WriteDescriptorBuffer` + `assign` ≥ 3% **e** worker ocupado ≤ 40% | 1b |
| worker ocupado > 40% já hoje | 1b só com 1c junto, ou pular 1b |
| espera da thread de trás pelo lock ≥ 3% | item L (seção 6) |
| caminho push ou set em draws com banco ≥ 30% desses draws | estender 1a/1b a eles (seção 5.3) |
| `Finish` por frame ≥ 1 ms | anotar: limita o ganho de 1b (H8) |
| (a) + `CurrentGraphicsPipeline` + estados dinâmicos + fora de draws ≥ 35% | B vale um estudo (seção 9) |
| 1a + 1b juntos < 5% | não fazer a fase 1; ir direto ao desenho de B ou C |

## 4. Fase 1a: identidade do payload, sem cópia nem comparação nos acertos

Só no caminho de descriptor buffer (`uses_descriptor_buffer`), que é onde ninguém lê o payload quando a
alocação é reaproveitada.

### 4.1 Ids de conteúdo

- `DescriptorCache` (`compact_texture_table.h:53`) ganha `u64 content_id`. Um contador global
  `u64 next_descriptor_content_id` (só a thread de trás escreve; nenhuma atômica) dá um id novo a **toda**
  mutação de `words`: o caminho de falha (`resize` + `memcpy`, `pipeline_helper.h:~505-520`) e o de patch
  (`PatchCompactElements`, `:450-480`). Para não esquecer nenhum lugar, `words` passa a ser escrito só por dois
  métodos (`Rewrite(...)` e `PatchElement(...)`) que trocam o id. Um `u64` não dá a volta, então não há ABA,
  mesmo se a tabela for liberada e outra nascer no mesmo endereço.
- Invariante: **mesmo `content_id` ⇒ mesmos bytes em `words`.** Ids diferentes não dizem nada (o conteúdo
  pode ser igual).

### 4.2 Por draw

- No acerto (`same_banks && !patched`), em vez de `AddEntries`: `guest_descriptor_queue.Skip(desc.count)`
  (avança o cursor sem escrever) e registra um **buraco** `{offset no payload, count, content_id, words.data()}`
  num `thread_local` pequeno por draw. Na patch e na falha, como hoje (escreve no payload).
- `ConfigureDraw`, caminho de descriptor buffer:
  1. Chave rápida: mesma geração do ring (como hoje) **e** mesma lista de buracos `(offset, count,
     content_id)` que a última alocação **e** `memcmp` só dos trechos fora dos buracos (buffers e elementos
     avulsos, poucos KB) contra `last_descriptor_payload`. Se tudo bate: reaproveita a alocação. Nada copiado.
  2. Se não bate: preenche os buracos (`memcpy` de `words`, ainda estável porque estamos na thread de trás no
     meio do draw) e faz o caminho de hoje: `memcmp` completo, e então reaproveita ou aloca, escreve e
     `assign`. Um conteúdo igual com id novo (geração de views trocada sem mudar os handles) continua sendo
     reaproveitado, como hoje.
  3. Guardar com a alocação: a lista de buracos e `last_descriptor_payload` completo (só quando aloca, como
     hoje).
- Custo: no acerto, ~0 em vez de ~150 KB de tráfego por draw com banco. Na falha, o de hoje mais o
  preenchimento dos buracos (a mesma cópia que `AddEntries` faria).

### 4.3 Riscos e cuidados

- **Payload com buracos é lixo para qualquer outro leitor.** Os buracos só existem com `uses_descriptor_buffer`
  e são preenchidos antes de qualquer leitura (passo 2). Com o caminho push ou set o modo 1a não se aplica:
  `AddEntries` como hoje. Assert no modo verify: um pipeline sem descriptor buffer nunca tem buracos.
- `rescaling.PushTexture(cache.rescaled)` continua no acerto (não depende da cópia).
- `Skip` precisa da mesma checagem de capacidade de `Acquire` (o `reserve` já cobre `num_descriptor_entries`).
- Lixo no slice: um buraco não preenchido deixa no slice os bytes de um frame anterior. Ninguém lê, mas no
  verify o slice é zerado antes para o teste diferencial dar resultado determinístico.

## 5. Fase 1b: escrita do descriptor buffer no worker

### 5.1 O que muda

Em `ConfigureDraw` (`vk_graphics_pipeline.cpp:1012-1029`), quando há alocação nova: a thread de trás faz
`Allocate`, preenche os buracos (1a), decide e guarda o estado de reaproveitamento, e **não** chama
`WriteDescriptorBuffer`. O lambda de draw que já é gravado (`:1065`) ganha três capturas: `entries`
(ponteiro para o slice do payload), `alloc.host` e `bool write_descriptors`. Ele chama
`WriteDescriptorBuffer(device, descriptor_buffer_layout, entries, host)` antes do
`SetDescriptorBufferOffsetsEXT`. O lambda continua pequeno (dezenas de bytes; o limite do chunk é 32 KB).

Compute (`vk_compute_pipeline.cpp:292`) fica fora no início (poucos dispatches); o mesmo padrão serve depois.

### 5.2 Por que é seguro (tempo de vida e ordem)

- `entries`: o slice do payload só é reescrito (a) depois de 8 `TickFrame` (`FRAMES_IN_FLIGHT`) ou (b) num
  estouro, que faz `WaitWorker` antes de reiniciar o cursor ou realocar. É o mesmo contrato em que o push
  descriptor e o `UpdateDescriptorSet` já confiam hoje. Com 1a, os buracos são preenchidos **antes** do
  `Record`, então o worker nunca lê `words` (que pode mudar no draw seguinte).
- `alloc.host`: o ring só reescreve a memória de um frame depois de `scheduler.Wait(frame_ticks[frame])`, um
  tick da GPU que só chega depois de o worker executar o lambda e submeter. Na volta dentro do mesmo frame,
  `Finish` espera a GPU, o que também implica o worker. A memória é host-coherent (o ring recusa a que não é):
  a escrita do worker antes do `vkQueueSubmit`, na mesma thread, fica visível sem flush.
- `descriptor_buffer_layout` e `device`: imutáveis depois da construção do pipeline. O pipeline vive enquanto o
  cache de pipelines vive (o lambda já captura `this` hoje).
- Handles de view e sampler dentro das entradas: `vkGetDescriptorEXT` passa a lê-los mais tarde, no worker. O
  risco é a view ser destruída entre o draw e a execução. Views apagadas vão para o `sentenced_image_view` (8
  frames). O atraso do worker é limitado pelo present a ~1-3 frames, e a fase 0 mede (3.4). O push descriptor
  já lê esses handles no worker hoje, então não há classe nova de risco, só mais draws expostos a ela.
- Reaproveitamento: a thread de trás pode decidir que o draw N+1 reaproveita a alocação X antes de o worker
  escrever X para o draw N. Não há problema: o lambda de N vem antes do de N+1 no fluxo, e a GPU só lê depois
  da submissão.
- Ordem com fences, queries, `Drain`, present e `TickFrame`: nada muda. Tudo que a GPU vê continua no mesmo
  fluxo de chunks, na mesma ordem. `Drain` continua garantindo só o que garante hoje (a thread de trás
  executou), e as leituras da CPU continuam esperando ticks da GPU, que já implicam o worker.
- Erros: `vkGetDescriptorEXT` é `void` e não lança. A falha de alocação continua decidida na thread de trás
  (`return false`, draw pulado, `submitted` como hoje).
- `vkGetDescriptorEXT` não exige sincronização externa (só lê os parâmetros e escreve no destino), e pode ser
  chamado de qualquer thread.

### 5.3 Extensão condicional (push e set com banco)

Só se a fase 0 mostrar draws com banco nesses caminhos. Eles precisam do payload contíguo no worker. Para tirar
a cópia da thread de trás, `words` teria de virar um snapshot imutável (copy-on-write: cada mutação aloca um
bloco novo numa arena por frame, liberada junto com o slice). É mais complexo e fica de fora até haver número.

## 6. Item L (condicional): estreitar o lock de `PrepareDraw`

Só se a espera da thread de trás pelo lock for ≥ 3%. Hoje o lock (`vk_rasterizer.cpp:278`) cobre
`Configure`, `UpdateDynamicStates`, queries, transform feedback, `draw_func` e `FlushStorageWrites`.
- Candidato: liberar o lock depois de `Configure` e do `PostCopyBarrier`, guardando antes o
  `texture_cache.IsRescaling()` (usado em viewport e scissor, `vk_rasterizer.cpp:~1351` e `:~1398`), para
  `Draw` não indireto. `DrawIndirect` e `DrawTexture` ficam como estão (usam o buffer cache no `draw_func`).
- A conferir antes: `query_cache.NotifySegment`, `CounterEnable` e `HandleTransformFeedback` pegam
  `buffer_cache.mutex` por dentro em alguns caminhos (`vk_query_cache.cpp:1410`, `:1444`, `:1501`, `:1641`);
  `FlushStorageWrites` mexe no runtime do buffer cache. Cada um fica dentro do lock ou ganha o seu.
- Semântica: entre o `Configure` e a gravação do draw, uma escrita da CPU do jogo pode marcar uma região já
  enviada para este draw. É equivalente à escrita chegar logo depois do draw (as duas coisas já são
  concorrentes no console). Os buffers não são destruídos na hora (destruição atrasada). Mesmo assim é uma
  mudança de semântica: revisar com cuidado e deixar atrás do interruptor.
- Ganho direto: só o tempo que a thread de trás espera. O ganho indireto (threads do jogo esperando menos) não
  aparece em draws/s se a thread de trás for o limite.

## 7. Sincronização, tempo e locks: invariantes e riscos

| # | Risco | O que garante | Como se verifica |
| --- | --- | --- | --- |
| H1 | Dois produtores no scheduler (corrupção silenciosa de `chunk`, `state`, `deferred_clear`) | R1: nada novo chama o scheduler fora da thread de trás | Assert de thread em `RecordWithUploadBuffer`, `RequestRenderpass`, `Update*`, `Flush`, `Finish` e `DispatchWork` com `EDEN_DRAW_EMIT_VERIFY=1` (guarda o id da thread no primeiro uso e loga uma vez se divergir; a thread do present e a de compilação não devem aparecer). |
| H2 | O worker lê memória já reescrita (payload, buracos, descriptor buffer) | 5.2; buracos preenchidos antes do `Record`; `WaitWorker` antes de reiniciar ou realocar o slice | Verify: a thread de trás guarda o hash (CityHash64) das entradas no lambda; o worker recalcula antes de escrever e loga "descriptor payload changed before the worker wrote it". Harness TSan (8.1). |
| H3 | Handle de view ou sampler destruído antes do `vkGetDescriptorEXT` | atraso do worker ≤ ~3 frames ≪ 8 frames de `sentenced_*` | Contador de atraso em frames (3.4) no log; no verify, aviso se passar de 4. Validation layers num teste curto, se o usuário topar. |
| H4 | Decisão de reaproveitamento errada (1a): id igual com conteúdo diferente | `words` só muda por `Rewrite`/`PatchElement`, que trocam o id; contador `u64` sem volta | Verify: em todo acerto da chave rápida, preenche os buracos e faz o `memcmp` completo mesmo assim; loga se der diferente. Teste diferencial (8.2). |
| H5 | Locks: inversão ou espera nova | O worker não pega lock de cache (R2); 1a e 1b não criam lock nenhum; a ordem `buffer → texture` continua | Revisão de código; contadores de espera dos dois lados (3.4) antes e depois. |
| H6 | Ordem de memória entre a thread de trás e o worker | O push e o pop do chunk sob `queue_mutex` (`DispatchWork`/`WorkerThread`) criam happens-before para tudo que a thread de trás escreveu antes do `Record` | TSan no harness (8.1), que usa a mesma fila. |
| H7 | Ordem com leituras da CPU, fences e queries | Inalterada: mesmo fluxo de chunks; fences por tick da GPU | Os pontos fixos de teste (seção 10) e `EDEN_DRAW_EMIT=0` como referência. |
| H8 | **Pontos seriais devolvem o trabalho movido**: `Finish` (uma vez por frame no `DownloadMemory`, mais as do texture cache), `WaitWorker` (estouro do payload, present sem thread de present) e `Wait(tick)` esperam a fila do worker, que agora tem mais trabalho | Inevitável com qualquer estágio posterior; o custo é o trabalho do worker ainda na fila nesse instante (≤ 32 draws de atraso no caso normal) | Medir o tempo de `Finish` e `WaitWorker` na thread de trás antes e depois (3.2, item 5). Se crescer muito, `DispatchWork` mais frequente antes de um `Finish` conhecido não adianta (o worker já está atrás); o remédio seria tirar o `Finish` do `DownloadMemory`, assunto à parte. |
| H9 | Latência de submissão maior | O worker faz mais por lote antes do `vkQueueSubmit` | Atraso da submissão (3.4). GPU a 67%: há folga; o jogo trava em 30 (intervalo 2), então alguns µs não importam. |
| H10 | SMT: o worker no mesmo núcleo físico da thread de trás rouba ciclos dela enquanto trabalha | Nada garante hoje (`SetCurrentThreadToPerformanceCores` não faz nada no Windows) | Experimento opcional `EDEN_GPU_AFFINITY=1` (só para medir): fixa a thread de trás e o worker em núcleos físicos diferentes (no Windows com Zen, os lógicos 2k e 2k+1 são irmãos). Se der diferença, discutir um padrão. Não ligar por padrão (as threads do jogo também disputam núcleo). |
| H11 | Crescimento do payload (`payload.assign`) com ponteiros vivos em lambdas | `WaitWorker` antes (já é assim) | Já coberto; o harness inclui crescimento. |
| H12 | Parar e reiniciar o split (`GpuSplit::Stop`, `MaybeStart` com as configurações mudando) | 1a e 1b não criam thread; o estado é por pipeline e independente do split | Teste manual: trocar a precisão da GPU no meio do jogo com o split ativo. |

Para 1c (se houver), os riscos novos são outros: `WaitWorker` tem de esperar os dois estágios (as duas filas
vazias e os dois `execution_mutex`); o estágio de preparo não pode usar o `cmdbuf`; e quem marca o chunk de
submissão é o estágio final. O desenho fica para quando houver número que o justifique.

## 8. Verificação

### 8.1 Harness TSan (fora da árvore, como os do split)

Modelo com o código real onde der: a fila de chunks copiada de `Scheduler::DispatchWork`/`WorkerThread`, e
`UpdateDescriptorQueue` compilado com stubs de `Device` e `Scheduler` (`WaitWorker` do modelo). O produtor grava
draws com payloads de tamanho aleatório, buracos e `TickFrame`, força estouros e crescimento, e reaproveita
alocações de um ring de descritores falso cujo "tick da GPU" anda quando o worker "submete". O worker confere o
hash de cada payload e escreve no ring falso; um verificador confere que nenhuma região do ring foi reescrita
antes de o tick dela passar. Rodadas com `sleep` aleatório nos dois lados, TSan e `-O2`, ~1 milhão de draws.

### 8.2 Teste diferencial de 1a (ASan/UBSan)

Sequências aleatórias de acertos, patches, falhas, troca de geração de view e troca de geração do ring,
aplicadas a (a) o caminho atual (`AddEntries` + `memcmp` completo) e (b) chave rápida + fallback. Exigências:
mesma decisão de reaproveitamento em todos os passos e payload final idêntico em toda alocação nova. Usar as
funções reais de `compact_texture_table.h`/`pipeline_helper.h` onde der, como no harness da tabela compacta.

### 8.3 Compilação

Como no pré-plano: `g++ -fsyntax-only -Wall` e o `clang++` com os avisos que o MSVC trata como erro, filtrando as
linhas alteradas, em `vk_graphics_pipeline.cpp` (instancia o `ConfigureImpl` e inclui `pipeline_helper.h`),
`vk_compute_pipeline.cpp` (também inclui `pipeline_helper.h`), `vk_scheduler.cpp`, `vk_rasterizer.cpp`,
`vk_update_descriptor.cpp`, `vk_descriptor_buffer.cpp`, `gpu_split.cpp` e `vk_texture_cache_base.cpp`. Cuidados
específicos: `/we5233` (uma captura usada só dentro de `if (verify)` conta como usada; uma usada só num
`if constexpr` descartado não conta), `/we4244` (contagens `size_t` → `u32`), `/we4456` (o `t` dos carimbos
encadeados).

### 8.4 No jogo

`EDEN_DRAW_EMIT_VERIFY=1` numa rodada curta pelos pontos fixos (devagar, só para pegar log). Depois as medições
com o verify desligado.

## 9. Fase 2 (B): o que decidir e o que já se sabe

Duas formas, para escolher com os números da fase 0 (baldes 2 e 3a contra o resto):

- **B-intra (a do pré-plano):** dividir o `PrepareDraw` em leitura (registradores, handles, `Build`) e
  resolução. Armadilhas no código: o `ConfigureImpl` chama `buffer_cache.DownloadMemory` no meio da
  decodificação (`~:501`, a tabela de handles escrita pela GPU), então a leitura de um draw não pode correr na
  frente da resolução do anterior quando os dados vêm da GPU. `texture_cache.SynchronizeDescriptors` (TIC/TSC
  da memória guest) abre o `ConfigureImpl`. As flags de sujeira de `maxwell3d->dirty` são consumidas e zeradas
  por vários leitores (pipeline cache, state tracker, estados dinâmicos), então um snapshot teria de levá-las
  junto.
- **B-split (segundo split, motor | rasterizador):** a thread de trás fica com os métodos da engine e o
  `DrawManager`, e uma thread nova fica com o `RasterizerVulkan` inteiro (caches, scheduler, query cache,
  fences). A interface entre as duas são as chamadas de `RasterizerInterface` (`Draw`, `Clear`,
  `DispatchCompute`, `Query`, `SignalFence`, `FlushRegion`...) viradas itens de ring, com os registradores
  replicados por log de escritas (como as cópias da frente para as macros). Reaproveita a máquina do `GpuSplit`
  (ring, `Drain`, `Defer`, harness TSan existente) e não divide o dono do scheduler. O custo é o balanceamento:
  com ~13% fora de draws na thread de trás e ~65% no `PrepareDraw`, o teto é ~×1,5 só se o estágio do
  rasterizador não crescer com a replicação dos registradores. Tudo que hoje lê o rasterizador de volta na
  hora (DMA que lê memória escrita pela GPU, semáforos, `AccelerateDMA` com retorno `bool`) vira `Drain` do
  estágio novo. Esse levantamento é o primeiro passo de B.

Decisão: se a fase 1 der ≥ +5% e nenhuma regressão, B-split é o candidato natural, com a fase 0 dizendo o
balanço. Se B não fechar no papel, C (heap bindless) volta a ser a alternativa.

## 10. Medição, aceite e entrega

- Métrica: draws/s no mesmo ponto da praça depois de estabilizar; média de 3 janelas de 4 s; o mesmo exe com
  `EDEN_DRAW_EMIT=0` e `=2`, e estatística de tempo desligada.
- Não pode piorar: palácio de Megaira, seleção de personagem, campo (~30) e uma batalha. Nem os tempos de
  `Finish` e `WaitWorker` (H8) a ponto de anular o ganho.
- Aceite para virar padrão: ≥ +3% de draws/s na praça, nada visual com o verify ligado, e o worker ocupado
  ≤ 50%. Abaixo disso o código fica atrás do interruptor, desligado.
- Branch: `perf/back-thread-split` a partir do `main`. Commits (em inglês, explicando o porquê):
  1. `video_core: time the GPU back thread by stage (EDEN_BACK_THREAD_STATS)`: fase 0.
  2. Rodada do usuário; a tabela da seção 3.5 entra no `docs/perf-handoff.md`.
  3. `vk_graphics_pipeline: reuse descriptor buffer allocations by content id` (1a) + verify.
  4. `vk_graphics_pipeline: write descriptor buffer entries on the Vulkan worker` (1b) + verify.
  5. Condicionais: L, 1c, 5.3.

## 11. Perguntas para a revisão

1. Algum consumidor do payload de `guest_descriptor_queue` além do lambda de draw e do `last_descriptor_payload`
   que eu não vi? (O buffer cache escreve nele em `BindHostStageBuffers`; até onde li, não lê.)
2. O caminho de descriptor buffer é mesmo o usado na RTX para os pipelines com banco? (`CanUseDescriptorBuffer`
   depende de `bufferlessPushDescriptors` e de `maxPushDescriptors`; a fase 0 responde, mas um palpite muda a
   ordem 1a/1b.)
3. Há algum caminho em que um `Finish` ou `Wait` da thread de trás aconteça **dentro** do lock dos caches com
   uma thread do jogo esperando por ele? Com mais trabalho no worker, essa espera cresceria (H8).
4. O limite de atraso do worker pelo present (`GetRenderFrame`) vale também com `use_present_thread` ligado e
   com o LSFG (frames gerados), ou existe um modo em que a thread de trás corre muitos frames na frente do
   worker?
5. Vale a pena guardar os **bytes** do descriptor buffer por bloco de tabela (cópia em vez de ~3000
   `vkGetDescriptorEXT` numa alocação nova)? Elimina em vez de mover; compete com 1b.
