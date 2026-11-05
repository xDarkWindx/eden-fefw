# Plano: dados que a GPU produz (Bananza) sem custo para o Fortune's Weave

Escrito em 2026-11-05 sobre `6d3dd9a` (branch `claude/peaceful-sagan-f9xjx7`, igual ao
`claude/upbeat-dijkstra-d3d1o6`). Nenhum jogo foi rodado para este plano: tudo vem da leitura do código
deste fork, do Eglantine (`Djihads80/Eglantine`, `nx2` em `e736dfd`) e do Lichii (`zortos293/Lichii`,
`nx2` em `8813b44`, fork do Eglantine que traz PRs a mais e uma pasta de estudos do Bananza).
Cada afirmação traz o grau de certeza: **verificado** (lido no código), **medido por eles** (número
dos commits ou docs dos forks, não reproduzido aqui) ou **hipótese**.

Documento para revisão crítica. As perguntas abertas estão na seção 7.

Este plano é o primeiro caso concreto de `docs/architecture-plan.md` (passo 2: tabela de estado de página
com o consumidor "busca de command list"). O desenho daqui deve seguir aquele, não o contrário.

## 0. Resumo

1. **Problema do Bananza:** o terreno destrutível some em Fast. Com a releitura dos parâmetros de macro
   (`EDEN_GPU_HIGH_PARTS=1`) ele aparece, mas **preto**. Em Accurate ele aparece certo.
2. **O 6d3dd9a corrige o caso errado e cobra do FE:** liga a releitura em todo jogo de Switch 2 e, para
   isso, tira as macros da thread da frente (o split cai do modo 2 para o 1). No FE a thread de trás é o
   gargalo, então as macros voltariam para o limite. Esse custo **ainda não foi medido**.
3. **Hipótese principal**, apoiada no código dos forks: o Bananza escreve **command lists inteiras com
   compute** (não só parâmetros de macro). Este fork executa a cópia das palavras feita na submissão
   (`submitted_words`) e só relê a memória com DMA Safe e split desligado, o que o Accurate liga. A releitura
   de macro conserta os parâmetros de macro dessas listas, mas os outros métodos delas (constant buffers,
   texturas, estado) continuam velhos. Isso explicaria tanto o chão invisível quanto o chão preto.
4. **O Eglantine já resolve esse padrão** de forma geral, pelo comportamento e não pelo título: um
   bitmap de páginas que a GPU escreveu, consultado pela frente sem esperar a de trás, e uma cópia da
   página no host feita logo depois de quem escreveu (seção 3.1).
5. **Proposta:** fazer primeiro um teste que confirma ou descarta a hipótese. Se confirmar, portar o mecanismo
   de listas escritas pela GPU e reverter o 6d3dd9a. Junto, há correções pequenas dos forks que valem para o
   FE e um plano de mais longo prazo (bindless) que ganha um primeiro passo barato.

## 1. Restrições

- **O FE não pode piorar.** A praça está em ~20 FPS porque a thread de trás trabalha ~43 ms por frame
  (`docs/back-thread-split-plan.md`, 3.14). O modo 2 do split existe para tirar as macros dela.
- **No FE, este fork é bem mais rápido que os forks de origem.** O usuário baixou e testou o Eglantine e o
  Lichii: no pior ponto da praça, ~14 FPS lá contra ~21 aqui (2026-11-05; versões e configurações dos
  builds não registradas). Por isso os forks servem de referência de **correção** (o que o Bananza precisa),
  não de desempenho. Nenhum mecanismo deles entra no caminho do FE sem A/B aqui, e as diferenças de
  desenho que custam tempo na thread de trás (esperas por entrada, flushes) são as primeiras suspeitas.
- Nada ligado por título. Cada correção é disparada pelo padrão que ela trata, e o caminho do FE só muda se
  o padrão aparecer nele.
- Toda mudança nova traz contador no `EDEN_BACK_THREAD_STATS`, variável para desligar (A/B no mesmo build) e
  medição de draws/s na praça e no palácio com as estatísticas desligadas (aceite da seção 10 do plano do
  split: ≥ 0% no FE para correções, ≥ +3% para otimizações).

## 2. O que este fork faz hoje (verificado)

| Ponto | Código | Comportamento |
| --- | --- | --- |
| Palavras de uma entrada do GPFIFO | `dma_pusher.cpp:141-155` | Usa `submitted_words` (cópia feita em `nvhost_gpu.cpp:400`, na submissão). Só relê a memória se `use_safe` e `IsMemoryDirty`. `use_safe` exige split desligado **e** DMA Safe (ou GPU Accurate com DMA Default). |
| Parâmetros de macro escritos pela GPU | `dma_pusher.cpp:121-138`, `maxwell_3d.cpp:266` | Só quando os parâmetros começam numa entrada nova: `IsMemoryDirty` marca a macro e `RefreshParametersImpl` relê com flush. Isso roda só na thread de trás; macros da frente nunca releem. |
| 6d3dd9a | `accuracy_parts.h`, `gpu_split.cpp:102` | Releitura ligada em todo título nativo; `front_macros` desligado se ela estiver ligada. |
| Leituras de memória por macro (FIFO DMA) | `maxwell_3d.cpp:1037` | A frente drena e lê (`DrainMacroMemory`). |
| Buffers escritos pela GPU lidos pela CPU do jogo | `buffer_cache_base.h:552-562`, `buffer_cache.h:186-200` | Já é adaptativo: só as faixas que a CPU leu (`cpu_read_ranges`) baixam nas fences. É o equivalente às armadilhas de leitura do Eglantine e ao #104 do Lichii. |
| Texturas escritas pela GPU lidas pela CPU | `vk_rasterizer.cpp:871` | Global: só com Accurate (ou bit 2 de `EDEN_GPU_HIGH_PARTS`). |

Consequência (**hipótese**, a ser confirmada no teste 4.1.1): uma command list que o compute escreve é
executada com as palavras de antes do compute, exceto os parâmetros de macro que a releitura conserta.

## 3. O que os forks têm e serve aqui

Lembrete da seção 1: os forks são ~14 FPS contra ~21 daqui na praça do FE. O que vem deles é tratado como
"o que precisa ser verdade para o Bananza ficar certo", e o desenho é refeito sobre o deste fork com o custo
medido no FE.

### 3.1 Command lists escritas pela GPU (Eglantine e Lichii) — central

- **Evidência de que o Bananza faz isso (medido por eles):** e3d8e9c (Lichii #70) conta "about 9k lists a
  second" do Bananza em páginas escritas por draws. 9479d8f (Lichii #54) mede 114-139 mil esperas por
  segundo no Bananza quando cada entrada drenava o split.
- **O mecanismo** (`src/video_core/nx2.h`, `dma_pusher.cpp` `IsGpuWrittenCommands`/`ReadGpuWrittenCommands`/
  `FlushGpuWrittenCommands` no Lichii):
  1. **Bitmap global de páginas escritas pela GPU** (`g_nx2_gpu_written_pages`): 1 bit por 4 KiB do espaço
     de dispositivo de 36 bits (2 MiB), átomos relaxados, marcado pelo buffer cache em `MarkWrittenBuffer`
     e nas cópias DMA. Os bits nunca são limpos, porque o dispatch que reescreve a página pode ainda
     estar na fila da thread de trás quando a frente lê a lista. A frente consulta sem lock e sem `Drain`.
  2. **Ordem com o split:** `NoteWriter()` anota a posição do ring a cada compute ou cópia enviada.
     `WaitForWriters()` espera a de trás só até lá, não a fila inteira. `NoteSync()`/`WaitForSync()` fazem
     o mesmo para esperas de host (syncpoint, WFI, bit de sync da entrada).
  3. **Caminho rápido (8f1435f, "read GPU-written command lists from copies instead of draining"):** a
     página de uma lista lida da GPU vira "página de command list". Depois de cada compute ou cópia que a
     escreve, o buffer cache copia a página para memória do host e submete. A frente lê a cópia quando a
     GPU termina, sem flush dos caches. Medido por eles em Mario Kart World: ~13.500 listas e ~400 idas e
     voltas à GPU por segundo eliminadas, ~40 → ~45 FPS.
  4. **Páginas escritas por draws (e3d8e9c):** só drenam se houve escrita desde o último flush; 94% das
     listas do Bananza nessas páginas não tinham escrita nova (medido por eles).
- **Neste fork:** nada disso existe (verificado: nenhum bitmap e nenhum caminho de leitura ao vivo com o
  split ligado).
- **Por que é a forma elegante:** a correção é disparada pela página ter sido escrita pela GPU, não pelo jogo.
  Corrige macros e métodos. As macros continuam na frente, porque a frente recebe as palavras certas antes
  de despachá-las. A releitura de macro do 6d3dd9a deixa de ser necessária.

### 3.2 Downloads pendentes com faixas próprias (Lichii 15126fb) — correção

`async_downloads` neste fork é um `OverlapRangeSet` único (verificado, `buffer_cache.h:682-723`). No Lichii,
um download mais novo da mesma faixa "fazia as vezes" do mais antigo: o pop do antigo escrevia bytes velhos e
tirava a faixa do `gpu_modified_ranges`. Em Hyrule Warriors isso virou contagens de vértices de bilhões e
device lost (medido por eles). Aqui só as faixas lidas pela CPU baixam, o que reduz a exposição, mas não a
elimina. **Candidato a port:** pequeno, sem custo no caminho comum.

### 3.3 Arrays em vez de hash em `VisitImageView`/`GetSamplerId` (Eglantine #256) — FE

Este fork ainda resolve índice → id por `image_view_ids`/`sampler_ids` (mapas, `texture_cache.h:330-344`,
`560-585`). O Eglantine usa `table.bound_ids[index]` (array do tamanho da tabela; ~2 MiB por tabela,
segundo a auditoria `docs/bananza-performance-candidates.md` do Lichii). No FE isso está no caminho quente
(~1250 views distintas por draw na praça). O reaproveitamento de bancos (100adbf) já evita parte das
chamadas, então o ganho é **incerto**: medir com `EDEN_TEXTURE_TABLE_STATS=1` quantas consultas sobram.
Também é o primeiro degrau do bindless (seção 4.5): o mesmo array vira o índice do heap.

### 3.4 Rede DLSS do FE (Eglantine, Duck912; Lichii `docs/native-dlss-replacement.md`) — margem de GPU

Este fork roda a rede DLSS do FE nativamente, com HMMA emulado (verificado: lowering em
`frontend/ampere/visitor.cpp`, sem substituto). O Eglantine/Lichii reconhecem a avaliação do FE (prepass e
shader de saída por identidade, `04007EF01F8AE000`) e trocam por upscale espacial com correção de jitter
(padrão), FSR 3.1 (sem runtime externo) ou DLSS de verdade via Streamline. Os docs deste fork estimam que a
GPU real fica perto de 100% a 30 FPS (3.14, restrição 4). **Primeiro medir** quanto da GPU a rede ocupa;
só então decidir. Ressalva: o reconhecimento é pelo contrato do FE, o que contraria a regra da seção 1. Se
entrar, fica como opção desligada por padrão.

### 3.5 Já cobertos ou não aplicáveis (conferido)

- Lichii #104 (baixar só as páginas Preflushable): este fork já baixa só `cpu_read_ranges`.
- Lichii #103 (escrita da CPU sobre página da GPU sem download): o `WriteMemory` daqui não baixa.
- Lote de fences (660e5f0): este fork tem `SignalFenceCoalesced` (`fence_manager.h:80`), com outro desenho.
- Tensor do Bananza (3944c47, 721b189): no Eglantine, sem ele a tela fica roxa. Ninguém relatou isso aqui.
  Conferir no log.
- Atômicos globais de contadores de append do Bananza (`docs/bananza-global-atomics.md` do Lichii): não
  existe aqui (`CollectAtomicPointers` ausente). Só importa se o log mostrar "Global atomic add with no
  routable storage buffer". Conferir no log.

### 3.6 Ferramentas

- Os contadores de diagnóstico do Lichii (`EDEN_NX2_CMDPAGE_DIAG`, `EDEN_DKB_FRAME_TRACE`) contam leituras
  por caminho (cópia, sincronizada, lenta) e escritas perdidas. Vale copiar a ideia para o
  `EDEN_BACK_THREAD_STATS`.
- Servidor de depuração opt-in com adaptador MCP (Lichii #111, `docs/nx2/debug-mcp.md`): um agente faz
  perguntas ao jogo rodando, sem relançar. Com ele desligado, o custo é um load atômico por frame. Serviria
  ao fluxo daqui (o usuário testa em casa, a sessão analisa). Fica para depois.

## 4. Plano

### 4.1 Fase A: command lists escritas pela GPU (substitui o 6d3dd9a)

**4.1.1 Teste que decide** (usuário, Bananza, sem código novo). PowerShell novo, GPU em **Fast**, lugar do
chão:

| Run | Configuração | Lê |
| --- | --- | --- |
| 1 | DMA Accuracy = Safe na interface; `EDEN_GPU_SPLIT=0`; `EDEN_NX2_MACRO_REFRESH=0` | Listas lidas ao vivo quando sujas; sem releitura de macro |
| 2 | DMA Accuracy = Default; `EDEN_GPU_SPLIT=0`; `EDEN_NX2_MACRO_REFRESH=1` | Só a releitura de macro (deve reproduzir o chão preto) |

- Run 1 com chão certo e colorido: a hipótese da seção 0 se confirma. Segue a 4.1.2.
- Run 1 com chão preto: as listas não explicam a cor. Volta a bissecção de `EDEN_GPU_HIGH_PARTS`
  (14, depois 2, 4 e 8) e a seção 4.4.
- Do FE, no mesmo build e com a GPU em Fast: praça com `EDEN_NX2_MACRO_REFRESH=0` e `=1`, para medir o custo
  real do 6d3dd9a.

**4.1.2 Desenho (port do Eglantine/Lichii, adaptado):**

1. `video_core/gpu_written_pages.h`: o bitmap da seção 3.1, limpo no início da sessão. É marcado em
   `BufferCache::MarkWrittenBuffer` e nas cópias DMA, sempre na thread de trás.
2. `GpuSplit`: `NoteWriter`/`WaitForWriters` e `NoteSync`/`WaitForSync` como no Lichii (posições do ring;
   esperas parciais com o mesmo giro e sono do `Drain`).
3. `DmaPusher`, ao escolher as palavras de uma entrada: se alguma página da entrada está no bitmap, a
   frente espera os escritores e lê ao vivo. Primeira versão (simples): espera, `IsMemoryDirty`, flush na
   thread de trás (como o `FlushGpuWrittenCommands`) e leitura. Segunda versão, se a primeira custar:
   cópias por página (8f1435f).
4. Reverter a parte do 6d3dd9a em `gpu_split.cpp` (macros voltam para a frente em todo título). Manter
   `RefreshParametersImpl` só com o bit 1 ou Accurate, como antes do 6d3dd9a. `EDEN_NX2_MACRO_REFRESH`
   continua existindo enquanto a fase A não for validada.
5. Contadores: entradas por caminho (submetida, ao vivo, cópia), esperas de escritores por segundo e o
   tempo delas. Chave: `EDEN_GPU_WRITTEN_LISTS=0`.

**4.1.3 O ponto delicado para o FE:** no Lichii, toda entrada que ainda não está no bitmap chama
`WaitForWriters()` (`dma_pusher.cpp`, logo antes da escolha do caminho). Isso existe para pegar a primeira
escrita, que talvez ainda esteja na fila de trás. No FE (~800 acquires por segundo esperando releases de
compute, segundo o 3.14 do plano do split; fila de trás quase sempre com trabalho) isso pode virar uma espera parcial por entrada e criar bolhas na thread
de trás, justamente a que limita. Duas opções, decididas pela medição:

- **A (exata, como o Lichii):** `WaitForWriters` em toda entrada. Correção na primeira escrita.
- **B (aprendida):** sem espera para páginas fora do bitmap; a página entra no bitmap quando a de trás
  executa o escritor. Uma página nova pode ser lida velha **uma vez** (um frame) antes de ficar marcada.
  Como as listas do Bananza são reescritas a cada frame nas mesmas páginas, o efeito seria um frame.
  Zero espera no FE se ele nunca ler listas escritas pela GPU.

Recomendação: implementar as duas atrás da chave (`=1` A, `=2` B), medir esperas por segundo no FE e no
Bananza e escolher o padrão pelos números. Ponto de partida: **B como padrão**. A é o desenho de forks que
rodam o FE a ~14 FPS contra ~21 aqui. Isso não prova que a espera por entrada é a causa da diferença, mas
basta para não adotá-la sem número. A só vira padrão se custar ~0 no FE.

**Riscos:** bitmap "para sempre" faz páginas reaproveitadas pelo jogo para outra coisa pagarem o caminho
lento (o Lichii trata isso relendo da memória enquanto nenhuma escrita nova acontecer). O flush roda na
thread de trás, não na frente, por causa do crash do 9960298/e3d8e9c (frente gravando no scheduler com a de
trás drenada). Há interação com entradas retomadas depois de um acquire (`command_resume_offset`).

### 4.2 Fase B: correções pequenas

- 15126fb (faixas por download), seção 3.2.
- #256 (arrays de ids), seção 3.3, só se as estatísticas mostrarem consultas restantes. Prioridade baixa:
  os forks têm o #256 e são mais lentos no FE; o reaproveitamento de bancos daqui já cobre o grosso.

### 4.3 Fase C: medir a rede DLSS do FE

Medir o tempo de GPU da rede na praça e numa batalha. O `EDEN_GPU_PROFILE` antigo saiu na limpeza de
diagnósticos (`docs/env-switches.md`), então a medição é com Nsight Graphics (GPU Trace) ou com um contador
novo de timestamps em volta dos dispatches da rede. Só se a rede passar de ~10% da GPU, abrir a discussão
da seção 3.4.

### 4.4 Fase D: coerência de texturas por dependência (só se a fase A não explicar a cor)

Hoje "textura escrita pela GPU lida pela CPU" é tudo ou nada. A versão por dependência copiaria o desenho
que os buffers já usam: marcar a região na primeira leitura da CPU depois de uma escrita da GPU e, dali em
diante, fazer flush só dela. Tem custo de rastreio; só entra com a bissecção apontando o bit 2 ou o 4.

### 4.5 Fase E (longa): heap bindless

Inalterada em relação à conversa anterior (o shader indexa um heap persistente pelo handle; as views são
atualizadas por evento e não por draw). Os forks não têm isso: o Eglantine usa "handle programs"
avaliados por draw (`shader_info.h`, `EvaluateHandleProgram`), que é outra forma do caminho atual. O #256
(seção 3.3) é o primeiro degrau. É a única mudança vista que mira os −25% da praça sozinha, e são semanas de
trabalho no recompilador.

### 4.6 Opcional: HLE das macros indiretas do NVN

Com a fase A, as macros recebem parâmetros certos; o HLE (draw indireto no Vulkan a partir do endereço dos
parâmetros, como o `HLE_DrawIndexedIndirect` do Switch 1, `macro.cpp:129`) tiraria também a ida e volta
pela CPU. Só vale se os contadores da fase A mostrarem muitas esperas no Bananza.

## 5. Ordem e aceite

1. Teste 4.1.1 (usuário). Decide a fase A e mede o 6d3dd9a no FE.
2. Fase A, com reversão do 6d3dd9a no split. Aceite: chão certo no Bananza em Fast. No FE, draws/s da praça
   e do palácio ≥ os de antes do 6d3dd9a, e contadores mostrando ~0 leituras ao vivo e ~0 esperas.
3. Fase B. Aceite: sem regressão; o #256 só com ganho medido.
4. Fase C (medição). A D e a E dependem dos números.

## 6. O que não fazer

- Ligar correções por título.
- Ligar partes do Accurate inteiras por padrão (o bit 2 inteiro, por exemplo) sem o padrão que as dispara.
- Portar em bloco: o Lichii e o Eglantine divergiram deste fork (`native_turing` e `NativeRootTables` aqui,
  `g_nx2_ampere` lá). Cada port é reescrito sobre o código daqui, citando o commit de origem.

## 7. Perguntas para o revisor

1. A hipótese da seção 0, item 3, se sustenta? Há outro caminho em Fast (além de `use_safe`) que leia listas
   ao vivo e que eu não tenha visto?
2. A opção B da 4.1.3 (um frame velho por página nova) é aceitável, ou existe um padrão de jogo em que a
   primeira leitura velha cause dano duradouro (lista com ponteiros, por exemplo)?
3. Marcar o bitmap só em `MarkWrittenBuffer` e nas cópias DMA cobre tudo? Escritas de imagem (render target
   ou storage image) sobre memória que depois é lida como lista não passam por ali.
4. Os bits nunca são limpos. Há jogo que reaproveita páginas escritas pela GPU para command lists da CPU em
   volume que torne isso caro?
5. O `RefreshParametersImpl` deve voltar ao estado de antes do 6d3dd9a (só com Accurate ou bit 1), ou ficar
   ligado para nativos como rede de segurança até a fase A provar que cobre tudo?
6. Os forks rodam o Bananza com o chão certo em Fast? Se sim, é evidência a favor da hipótese (eles têm o
   mecanismo de listas escritas pela GPU e não têm a releitura de macro fora do Accurate). O usuário pode
   conferir com os builds que já baixou.

## 8. Referências

| Origem | Commit / doc | Assunto |
| --- | --- | --- |
| Lichii | 8f1435f | Listas escritas pela GPU lidas de cópias |
| Lichii | e3d8e9c (#70) | Páginas escritas por draws só drenam depois de escrita; trabalho de fence na de trás |
| Lichii | 9479d8f (#54) | Sem `Drain` por entrada; `WaitForWriters` |
| Lichii | 9960298 (#59) | Flush da frente executado na de trás |
| Lichii | 15126fb | Faixas por download pendente |
| Lichii | 0bf0f83 (#104), 3165a2b (#103) | Preflush exato; escrita da CPU sem download (já cobertos) |
| Eglantine | #256 (merge 47882d1) | `bound_ids` em vez de mapas |
| Eglantine | 3944c47, 721b189, fef7d2f | Programas tensor do Bananza |
| Lichii | `docs/native-dlss-replacement.md`, `docs/FE_UPSCALE_STANDIN.md` | Substituição da rede DLSS do FE |
| Lichii | `docs/bananza-*.md` | Diagnósticos do Bananza (ID `040074A01BF12000`) |
| Lichii | `docs/nx2/debug-mcp.md` (#111) | Servidor de depuração e MCP |
| Este fork | 6d3dd9a, c2a5438 | Releitura de macro e `EDEN_GPU_HIGH_PARTS` |
