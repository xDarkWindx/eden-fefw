# Plano: dados que a GPU produz (Bananza) sem custo para o Fortune's Weave

Escrito em 2026-11-05 sobre `6d3dd9a` (branch `claude/peaceful-sagan-f9xjx7`). Nenhum jogo foi rodado para
este plano: tudo vem da leitura do código deste fork, do Eglantine (`Djihads80/Eglantine`, `nx2` em `e736dfd`)
e do Lichii (`zortos293/Lichii`, `nx2` em `8813b44`).
Graus de certeza: **verificado** (lido no código), **medido** (número de um commit ou doc, não reproduzido
aqui) e **hipótese**.

**Revisão 1 (2026-11-05):** incorpora a revisão estática feita sobre `af61d42`. As afirmações dela foram
conferidas no código; o que mudou está na seção 9, e as seções 0, 2, 3.1, 4.1, 5 e 7 foram corrigidas no
lugar. A principal mudança: a opção "aprendida" para command lists saiu, e o teste virou diagnóstico.

Este plano é o primeiro caso concreto de `docs/architecture-plan.md`. O desenho daqui segue aquele.

## 0. Resumo

1. **Problema do Bananza:** o terreno destrutível some em Fast. Com a releitura dos parâmetros de macro
   (`EDEN_GPU_HIGH_PARTS=1`) ele aparece, mas **preto**. Em Accurate ele aparece certo.
2. **O 6d3dd9a liga a releitura em todo jogo de Switch 2** e, para isso, tira as macros da thread da frente
   (verificado: `gpu_split.cpp:102-103`; a variável vale para o processo todo). No FE a thread de trás é o
   gargalo. Esse custo **ainda não foi medido**.
3. **Hipótese:** parte das command lists que o Bananza executa é produzida pela GPU, e a versão que chega ao
   parser em Fast é anterior ao produtor. O código tem **três fontes** de palavras para uma entrada
   (verificado, `dma_pusher.cpp:141-163`):
   - **snapshot da submissão** (`submitted_words`), o caso comum;
   - **memória atual sem flush** (`UnsafeRead`), quando a entrada é retomada depois de um acquire
     (`command_resume_offset != 0`) ou não tem snapshot;
   - **memória atual com flush** (`SafeRead`), só com split desligado e DMA Safe (ou Accurate).
   Ler "ao vivo" não quer dizer coerente com a GPU: a segunda fonte pode ler bytes velhos (o host ainda não
   devolveu o que a GPU escreveu) ou novos demais (o jogo já reaproveitou a memória para o frame seguinte,
   que é o motivo do snapshot, `nvhost_gpu.cpp:392-397`).
   A hipótese **não está demonstrada**: falta saber quais palavras o Bananza produz, quem as escreve e que
   versão chega ao parser.
4. **Proposta:** primeiro um build de diagnóstico que compara, por entrada, o snapshot com a versão coerente
   e diz o que difere (seção 4.1). Só com esse resultado se decide o consumidor, e o desenho dele segue um
   protocolo exato (seção 4.2), sem execução especulativa de comandos.

## 1. Restrições

- **O FE não pode piorar.** A praça está em ~20 FPS porque a thread de trás trabalha ~43 ms por frame
  (`docs/back-thread-split-plan.md`, 3.14). O modo 2 do split existe para tirar as macros dela.
- **No FE, este fork é bem mais rápido que os forks de origem.** O usuário testou o Eglantine e o Lichii:
  no pior ponto da praça, ~14 FPS lá contra ~21 aqui (2026-11-05; versões e configurações não registradas).
  Os forks servem de referência do que o Bananza precisa, não de desenho. Nada deles entra no caminho do FE
  sem A/B aqui.
- Nada ligado por título. O FE é protegido por **classificação correta** (o caminho caro só roda onde a
  GPU produziu os dados), não por aceitar dados possivelmente velhos.
- Toda mudança traz contador no `EDEN_BACK_THREAD_STATS`, variável para desligar e medição de draws/s na
  praça e no palácio com as estatísticas desligadas.

## 2. O que este fork faz hoje (verificado)

| Ponto | Código | Comportamento |
| --- | --- | --- |
| Fonte das palavras de uma entrada | `dma_pusher.cpp:141-163` | Três fontes (seção 0, item 3). Existe ainda a lista de prefetch, executada da cópia própria (`:101-107`). |
| Snapshot | `nvhost_gpu.cpp:392-406` | Só em canais com ABI nativa. Feito porque o jogo reescreve memória de comandos já submetida quando roda à frente da GPU emulada. |
| Parâmetros de macro escritos pela GPU | `dma_pusher.cpp:121-138`, `maxwell_3d.cpp:266` | `IsMemoryDirty` quando os parâmetros começam numa entrada nova; `RefreshParametersImpl` relê com flush. Só na thread de trás. |
| 6d3dd9a | `accuracy_parts.h`, `gpu_split.cpp:102-103` | Releitura em todo título nativo; macros fora da frente. |
| `SafeRead` | `memory_manager.cpp:409-444` | `ReadBlockImpl` chama `FlushRegion`, que baixa **texturas e buffers** da região (`vk_rasterizer.cpp:841-856`). Ou seja, o modo Safe também muda leituras de textura e o tempo. |
| Ordem das esperas de host na frente | `puller.cpp` | Acquires de semáforo drenam (`:151`, `:186`, `:285`). Acquire de syncpoint (`:126-132`) e WFI (`:322-323`) vão para a thread de trás **sem** drenar. O bit de sync da entrada só drena com "Sync Memory Operations" ligado (`dma_pusher.cpp:173-183`). |
| Primeira leitura da CPU de buffer escrito pela GPU | `vk_rasterizer.cpp:883-908`, `gpu.cpp:257-272` | No Vulkan, só registra a leitura (`NoteCpuRead`) e retorna `preemtive = true` para buffers: essa primeira leitura não baixa nada, e as fences seguintes baixam a faixa. É o único "aprendido" que existe hoje, e é para dados lidos pela CPU, não para comandos. |

## 3. O que os forks têm e serve aqui

Os forks são ~14 FPS contra ~21 daqui na praça do FE. O que vem deles é tratado como pista do que precisa
ser verdade para o Bananza, e o desenho é refeito aqui com o custo medido no FE.

### 3.1 Command lists escritas pela GPU (Eglantine e Lichii)

- **Evidência externa (medida por eles, não reproduzida):** e3d8e9c (Lichii #70) conta "about 9k lists a
  second" do Bananza em páginas **escritas por draws**. Isso não prova que as listas inteiras vêm de compute,
  nem qual parte delas difere.
- **O mecanismo deles:** bitmap de páginas que a GPU escreveu (marcado pelo buffer cache, nunca limpo),
  `WaitForWriters` em toda entrada fora do bitmap, e leitura da memória atual (ou de uma cópia feita depois
  do escritor) para entradas no bitmap (8f1435f, 9479d8f, e3d8e9c).
- **Problemas do mecanismo deles para este fork (revisão, conferidos):**
  1. Ler a memória atual para toda página marcada pode trazer palavras **novas demais** (o motivo do
     snapshot daqui). Um bit "a GPU já escreveu esta página" não distingue lista produzida agora pela GPU,
     lista da CPU numa página que a GPU usou antes, outros dados na mesma página ou memória reaproveitada.
  2. `WaitForWriters` em toda entrada multiplica as esperas na thread que limita o FE.
  3. Marcar páginas em cada escrita de storage ou render target acontece **no caminho de draws**.
- **A ideia que fica:** a versão certa de uma entrada é "o snapshot da submissão, com as faixas que
  produtores anteriores ao fetch escreveram". E3d8e9c dá a pista para a ordem: no hardware, o que um draw
  escreve só chega ao fetch de uma lista depois de uma espera de host (acquire de semáforo, acquire de
  syncpoint, WFI, bit de sync), porque o GPFIFO busca à frente. Isso vira o protocolo da seção 4.2
  (**hipótese**, a confirmar com o diagnóstico).

### 3.2 Downloads pendentes com faixas próprias (Lichii 15126fb)

`async_downloads` é um `OverlapRangeSet` compartilhado (verificado, `buffer_cache.h:682-723`), ao lado de
filas de cópias por download (`pending_downloads`). O problema que o Lichii corrigiu está em validar e
aposentar os downloads pelo conjunto compartilhado. Aqui só as faixas lidas pela CPU baixam, o que reduz a
exposição sem eliminá-la. É independente do Bananza: primeiro um teste que reproduza o caso (download mais
novo da mesma faixa antes do pop do antigo), depois o port.

### 3.3 Arrays em vez de mapas em `VisitImageView`/`GetSamplerId` (Eglantine #256)

Os mapas estão confirmados (`texture_cache.h:340-343`, `576-582`). O ganho no FE não segue disso: os bancos
reaproveitados (100adbf) já evitam muitas consultas, e os forks têm o #256 e são mais lentos no FE. Só com
`EDEN_TEXTURE_TABLE_STATS=1` mostrando consultas restantes.

### 3.4 Rede DLSS do FE

Este fork lowera HMMA (verificado: `frontend/ampere/visitor.cpp:484`); isso não prova que toda a rede do FE
roda só por esse caminho. Os forks trocam a avaliação do FE por upscale espacial, FSR 3.1 ou DLSS, por
contrato do FE. Primeiro medir quanto da GPU a rede ocupa (Nsight Graphics ou timestamps em volta dos
dispatches; o `EDEN_GPU_PROFILE` antigo saiu). Se entrar, é um plugin desligado por padrão.

### 3.5 Já cobertos ou fora de escopo

- Lichii #104: este fork já baixa só `cpu_read_ranges` (com escopo: downloads explícitos continuam).
- Lichii #103: o `WriteMemory` daqui não baixa.
- Lote de fences (660e5f0): este fork tem `SignalFenceCoalesced`.
- Tensor e atômicos de append do Bananza: conferir no log ("Global atomic add with no routable storage
  buffer", tela roxa). Os atômicos globais de 64 bits lançam exceção nos emissores citados
  (`emit_spirv_atomic.cpp:473-485`); não vale para todos os atômicos de storage de 64 bits.

### 3.6 Ferramentas

Contadores por caminho de leitura (`EDEN_NX2_CMDPAGE_DIAG` do Lichii) e servidor de depuração com MCP
(Lichii #111) como ideias para depois.

## 4. Plano

### 4.1 Diagnóstico de command lists (antes de mudar qualquer fonte)

**4.1.1 Matriz de testes (usuário, Bananza, sem código novo).** PowerShell novo para cada linha, GPU em
**Fast**, `EDEN_GPU_SPLIT=0` em todas, o resto constante, o mesmo lugar e a mesma câmera:

| | `EDEN_NX2_MACRO_REFRESH=0` | `EDEN_NX2_MACRO_REFRESH=1` |
| --- | --- | --- |
| DMA Accuracy = Default | referência (esperado: chão invisível) | só macros (esperado: chão preto) |
| DMA Accuracy = Safe | listas com flush, sem macro | as duas |

Isso diz se o modo Safe conserta a cor, mas **não confirma a causa**: o Safe passa por `FlushRegion`, que
baixa também texturas e muda o tempo (seção 2). Uma melhora pode vir disso.

**4.1.2 Build de diagnóstico (desligado por padrão, `EDEN_LIST_DIAG=1`).** Para cada entrada executada
pelo snapshot cujas páginas tenham faixas modificadas pela GPU:

1. A **thread de trás** (dona dos caches) obtém, na posição da entrada, as faixas modificadas pela GPU
   (`gpu_modified_ranges`) e baixa só essas faixas do buffer do host. Ela publica o resultado para a frente,
   que nunca consulta caches (seção 9, item 6).
2. Compara com o snapshot. Para cada diferença, registra: deslocamento na entrada; se cai num cabeçalho, em
   dados de método ou em parâmetros de macro; o método e o subcanal; o tipo do último produtor (compute,
   draw, cópia), a posição dele no ring; e a última espera de host antes da entrada (tipo e posição).
3. Log limitado (primeiras N entradas, depois amostragem) e contadores por segundo: entradas com faixas da
   GPU, entradas com bytes diferentes, bytes diferentes em cabeçalhos/métodos/macros.
4. Em modo diagnóstico, **não muda o que é executado**.

O mesmo build no FE mede o custo do filtro (quantas entradas caem em páginas com faixas da GPU). Se for ~0,
o consumidor da seção 4.2 é grátis no FE.

**4.1.3 O que decide:** diferenças em cabeçalhos ou dados de método (não só em parâmetros de macro),
precedidas por uma espera de host, nas entradas dos draws do terreno, confirmam a hipótese e o protocolo.
Sem diferenças fora de macros, a causa do chão preto está em outro lugar (bissecção de `EDEN_GPU_HIGH_PARTS`
com 14, 2, 4 e 8, e a seção 4.4 do plano de arquitetura).

### 4.2 Consumidor de command lists (só depois do diagnóstico; opt-in até validar)

**Versão correta de uma entrada:** o snapshot da submissão com as faixas escritas por produtores que vêm
antes do fetch na ordem da GPU. Não a memória atual. Isso evita tanto os bytes velhos quanto os novos demais.

**Protocolo mínimo (exato, sem especulação):**

1. **Filtro conservador:** bitmap de páginas com faixas modificadas pela GPU, 1 bit por 4 KiB (2 MiB),
   marcado pela thread de trás quando executa uma escrita da GPU. Bit desligado = a página certamente não
   tem dado da GPU **entre os produtores já executados**. Ele é só um filtro: a autoridade sobre quais
   bytes são da GPU continua nos caches (`gpu_modified_ranges`, faixas exatas).
2. **Produtor pendente:** um produtor ainda na fila de trás não marcou o bitmap. A regra vem do hardware
   (3.1): o produtor só é visível ao fetch depois de uma espera de host. A frente anota as esperas de host
   que **não** drenam hoje (acquire de syncpoint, WFI, bit de sync sem "Sync Memory Operations"). Na
   primeira entrada depois de uma espera nova, ela espera a thread de trás passar dessa espera (uma vez por
   espera, não por entrada) e só então consulta o bitmap. Os acquires de semáforo já drenam.
3. **Execução do produtor versus GPU:** a thread de trás ter passado do produtor não quer dizer que a GPU
   terminou. A cópia das faixas é gravada no fluxo de comandos depois do produtor e lida só quando a fence
   dela concluir.
4. **Publicação:** a thread de trás grava as faixas num buffer próprio da entrada e publica com
   release/acquire. A frente sobrepõe as faixas ao snapshot e executa. Nenhuma versão "de página" é marcada
   como atual a partir de uma cópia parcial.
5. **Entradas retomadas depois de um acquire** (`command_resume_offset != 0`) seguem a mesma regra, em vez
   da memória atual sem flush. Isso muda um caminho existente: fica atrás da chave e com contador.

**Custo no FE:** bitmap consultado por página de cada entrada (leitura atômica, barata mas não zero) e
uma espera por espera de host não drenada. As duas coisas são medidas pelo diagnóstico antes.

**Riscos e pontos abertos:**
- A regra do item 2 é hipótese. Se o diagnóstico mostrar entradas com diferenças **sem** espera de host
  antes, o protocolo não basta, e a alternativa conservadora é esperar a de trás em toda entrada que cai no
  bitmap (não em toda entrada).
- Produtores que não passam pelo buffer cache (imagens, render targets, storage images com alias, queries,
  escritas de HLE) não marcam o bitmap. O diagnóstico precisa de um inventário deles (seção 7, item 3).
- Bits nunca limpos geram falsos positivos (páginas reaproveitadas). Medir páginas marcadas contra entradas
  com bytes realmente diferentes.

**Chave:** `EDEN_GPU_LISTS=0/1` (padrão 0 até validar no Bananza e no FE).

### 4.3 Releitura de macro

Fica como está (opt-in para o FE: `EDEN_NX2_MACRO_REFRESH=0` restaura as macros na frente) até o consumidor
da 4.2 provar que cobre os parâmetros de macro, incluindo parâmetros que atravessam entradas e retomadas
depois de acquire. Depois disso, o 6d3dd9a sai e a releitura volta ao Accurate e ao bit 1. O override de
diagnóstico continua.

### 4.4 Correções pequenas

15126fb (seção 3.2) com teste próprio. #256 (seção 3.3) só com ganho medido.

### 4.5 Medir a rede DLSS do FE

Seção 3.4.

## 5. Ordem e aceite

1. Matriz 4.1.1 (usuário) e, no mesmo build, FE na praça com `EDEN_NX2_MACRO_REFRESH=0` e `=1`.
2. Build de diagnóstico 4.1.2 (código), no Bananza e no FE.
3. Teste e port do 15126fb (independente).
4. Protocolo e consumidor 4.2, opt-in. Aceite: chão certo no Bananza em Fast; no FE, draws/s da praça e do
   palácio ≥ os de antes do 6d3dd9a; contadores com ~0 entradas filtradas no FE.
5. Só então: reverter o 6d3dd9a e decidir o padrão do consumidor.

## 6. O que não fazer

- Ligar correções por título.
- Executar comandos possivelmente velhos como política padrão.
- Ler a memória atual de uma lista sem saber qual versão ela representa.
- Consultar ou fazer flush dos caches na frente depois de uma espera parcial.
- Portar em bloco dos forks.

## 7. Perguntas abertas (revisão 1)

1. A regra "produtor só é visível ao fetch depois de uma espera de host" vale para o NVN? O diagnóstico
   responde no Bananza; vale conferir em mais jogos.
2. A sobreposição "snapshot + faixas da GPU" erra quando a CPU do jogo reescreve, depois da submissão, uma
   faixa que a GPU produziu antes do fetch. No hardware isso seria uma corrida do próprio jogo. Aceitável?
3. Inventário de produtores: storage buffers de draws e compute, imagens, storage images e render targets
   com alias, queries, escritas de CPU e HLE, cópias, mudanças de mapeamento. Quais podem produzir comandos?
4. O caminho de entradas retomadas (`UnsafeRead`) já causa problemas hoje em algum jogo? O diagnóstico pode
   contar diferenças entre a memória atual e a versão coerente nessas entradas.
5. Os forks desenham o chão do Bananza em Fast? Ajuda, mas não confirma (diferem em shaders, coerência e
   sincronização).

## 8. Referências

| Origem | Commit / doc | Assunto |
| --- | --- | --- |
| Lichii | 8f1435f | Listas escritas pela GPU lidas de cópias |
| Lichii | e3d8e9c (#70) | Páginas escritas por draws; esperas de host antes do fetch |
| Lichii | 9479d8f (#54) | Sem `Drain` por entrada; `WaitForWriters` |
| Lichii | 9960298 (#59) | Flush da frente executado na de trás |
| Lichii | 15126fb | Faixas por download pendente |
| Lichii | 0bf0f83 (#104), 3165a2b (#103) | Já cobertos |
| Eglantine | #256 (merge 47882d1) | `bound_ids` em vez de mapas |
| Lichii | `docs/native-dlss-replacement.md`, `docs/FE_UPSCALE_STANDIN.md` | Rede DLSS do FE |
| Lichii | `docs/bananza-*.md` | Diagnósticos do Bananza (ID `040074A01BF12000`) |
| Este fork | 6d3dd9a, c2a5438 | Releitura de macro e `EDEN_GPU_HIGH_PARTS` |

## 9. Revisão 1: o que mudou e por quê

| # | Ponto da revisão | Conferido | Mudança |
| --- | --- | --- | --- |
| 1 | Fast também lê ao vivo (retomada, sem snapshot) | Sim, `dma_pusher.cpp:141-163` | Três fontes na seção 0 e na 2; a hipótese foi reescrita e rebaixada |
| 2 | "Aprendido" não erra só um frame: o parser guarda estado entre entradas | Sim, `dma_pusher.cpp:121-138` e `ProcessCommands` | Opção B removida; protocolo exato na 4.2 |
| 3 | Memória atual pode ser nova demais | Sim, `nvhost_gpu.cpp:392-397` | Versão certa = snapshot + faixas da GPU |
| 4 | Teste não isola a causa; Safe também faz flush de texturas | Sim, `memory_manager.cpp:409-444`, `vk_rasterizer.cpp:841-856` | Matriz 2x2 e build de diagnóstico |
| 5 | Versões e átomos relaxados não formam protocolo | Sim | Bitmap é só filtro; autoridade nos caches; publicação e conclusão explícitas |
| 6 | Espera parcial não autoriza a frente a usar caches | Sim, `memory_manager.cpp:518`, `vk_rasterizer.cpp:862-878` | Toda consulta e flush pela thread de trás, com resultado publicado |
| — | "Primeira leitura velha da CPU" não demonstrado | **Discordo em parte:** no Vulkan, `GetFlushArea` retorna `preemtive = true` para buffers (`vk_rasterizer.cpp:883-908`) e `OnCPURead` não baixa nada nesse caso (`gpu.cpp:257-272`). O `BufferCache::GetFlushArea` citado na revisão é usado pelo OpenGL (`gl_rasterizer.cpp:523`). | Mantido, com escopo: vale para dados lidos pela CPU no Vulkan e **não** justifica nada para comandos |
| — | Texturas: o bit 2 limita a detecção automática, não o download explícito | Sim, `vk_rasterizer.cpp:845-850` | Seção 2 corrigida |
| — | Memória global escolhe a base mais próxima abaixo do endereço | Sim, `spirv_emit_context.cpp:976` | Levado ao plano de arquitetura |
