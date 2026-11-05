# Plano: dados que a GPU produz (Bananza) sem custo para o Fortune's Weave

Escrito em 2026-11-05 sobre `6d3dd9a` (branch `claude/peaceful-sagan-f9xjx7`). Nenhum jogo foi rodado para
este plano: tudo vem da leitura do código deste fork, do Eglantine (`Djihads80/Eglantine`, `nx2` em `e736dfd`)
e do Lichii (`zortos293/Lichii`, `nx2` em `8813b44`).
Graus de certeza: **verificado** (lido no código), **medido** (número de um commit ou doc, não reproduzido
aqui) e **hipótese**.

**Revisão 1 (2026-11-05):** incorpora a revisão estática feita sobre `af61d42`. As afirmações dela foram
conferidas no código; o que mudou está na seção 9, e as seções 0, 2, 3.1, 4.1, 5 e 7 foram corrigidas no
lugar. A principal mudança: a opção "aprendida" para command lists saiu, e o teste virou diagnóstico.

**Revisão 2 (2026-11-05):** incorpora a segunda revisão, sobre `ba797e4`, conferida no código. O que mudou
está na seção 10; as seções 0, 4.1, 4.2, 5, 6 e 7 foram corrigidas no lugar. A principal mudança: as
dependências de host precisam ser satisfeitas, não só processadas, e o consumidor só vem depois de casos
dirigidos.

**Revisão 3 (2026-11-05):** incorpora o resultado do harness dos contratos (`tools/gpu-contract-harness/`,
tarefa T1) e a revisão dele. O que mudou está na seção 11; as seções 4.2, 5 e 7 foram corrigidas no lugar.
A principal mudança: `exact-drain` passa a ser a regra exata e o padrão do consumidor; a regra "depois de
dependência satisfeita" vira otimização condicionada, desligada por padrão.
Atualização (mesma data): o item 2 da 4.2 separa o valor do syncpoint, o processamento do produtor pela
trás e a conclusão na GPU, e corrige quando o host sobe (seção 11, linha 9).

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
4. **Proposta:** primeiro um build de diagnóstico que compara, por entrada, as palavras usadas com a versão
   coerente e diz o que difere (seção 4.1), e casos dirigidos num harness (4.1.4). Só depois vem o consumidor
   (seção 4.2), sem execução especulativa de comandos; ele só pode ser chamado de exato quando os casos
   dirigidos passarem.

## 1. Restrições

- **Segurança antes de desempenho, com orçamento (diretriz do usuário, 2026-11-05).** Uma pequena regressão
  no FE é aceitável em troca de algo mais seguro e agnóstico, dentro do orçamento de
  `docs/architecture-plan.md`, seção 12.1 (proposta: até 5% de draws/s por item). A praça está em ~20 FPS
  porque a thread de trás trabalha ~43 ms por frame (`docs/back-thread-split-plan.md`, 3.14); o modo 2 do
  split existe para tirar as macros dela, e o custo de cada passo continua medido.
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

**4.1.2 Build de diagnóstico (desligado por padrão, `EDEN_LIST_DIAG=1`).** Observa sem mudar o que é
executado. Para cada entrada em página com faixas modificadas pela GPU, executada pelo snapshot **ou** pela
memória atual (entradas retomadas, `UnsafeRead`):

1. **Versão coerente na posição da entrada:** a frente enfileira um pedido no ring junto com a entrada; a
   thread de trás o grava no fluxo do Vulkan como cópia GPU → buffer privado do diagnóstico, depois de todo
   o trabalho enviado antes da entrada e antes de qualquer coisa enviada depois. Não baixa para a memória do
   jogo nem muda `gpu_modified_ranges`. O resultado é lido de forma assíncrona, frames depois. Isso reduz a
   perturbação, mas não a zera: há barreiras e cópias a mais no fluxo, e o tempo muda.
2. **Comparação:** palavras usadas pelo parser contra a versão coerente. Para cada diferença: deslocamento;
   cabeçalho, dados de método ou parâmetros de macro; método e subcanal; tipo e posição do último produtor
   da faixa; última espera de host antes da entrada (tipo, condição e posição).
3. **Escrita perdida (ponto cego da primeira escrita):** na posição da entrada, a thread de trás confere se
   algum produtor executado **depois** de a frente ter lido a entrada (enviado antes dela, ainda na fila no
   momento da leitura) escreveu nas páginas da entrada. Conta esses casos mesmo quando a página não estava
   marcada na leitura (ideia do `CheckMissedWrite` do Lichii, e3d8e9c).
4. **Pontos cegos que continuam:** produtores fora do buffer cache (imagens, render targets, storage images
   com alias, queries, escritas de HLE). O diagnóstico lista os que conseguir identificar e diz o que não
   cobre.
5. Log limitado (primeiras N entradas, depois amostragem) e contadores por segundo.

No FE, o mesmo build mede: entradas candidatas, marcações por segundo no caminho de draws, consultas por
página e esperas de host que o protocolo da 4.2 acrescentaria. **~0 entradas candidatas elimina quase todo o
custo das transferências; o custo fixo (marcações e consultas) e o das esperas precisa ser medido.**

**4.1.3 O que o diagnóstico pode e não pode concluir.** Diferenças em cabeçalhos ou dados de método,
precedidas por uma espera de host, nas entradas dos draws do terreno, **sustentam** a hipótese nesses casos.
Não demonstram que toda produção relevante passa por uma espera, nem que essas diferenças causam o chão
preto. A causalidade só vem com o consumidor em modo experimental. Escritas perdidas (item 3) sem espera de
host antes derrubam a regra da 4.2, item 2. Sem diferenças fora de macros, a causa está em outro lugar
(bissecção de `EDEN_GPU_HIGH_PARTS` com 14, 2, 4 e 8).

**4.1.4 Casos dirigidos (antes do consumidor).** Um harness fora da árvore, como os do split (ring real,
`DmaPusher` e puller reais, buffer cache e GPU falsos com produtores roteirizados), com a versão esperada de
cada entrada em cada caso:

| Caso | Roteiro | O que precisa sair certo |
| --- | --- | --- |
| Primeira escrita | página nunca marcada; produtor na fila quando a frente lê a entrada consumidora | a entrada vê o produtor (ou o caso é detectado e contado) |
| Produtor de outro canal | produtor no canal A, consumidor no canal B depois de um acquire de syncpoint | a entrada só executa depois de a condição do acquire ser satisfeita |
| Retomada dentro da entrada | acquire no meio da entrada; produtor antes do release | as palavras depois do acquire são a versão coerente |
| Reuso pela CPU | o jogo observa a conclusão (sinalizada antes da hora) e reescreve a memória submetida | a entrada executa a versão da submissão mais o produtor, não a reescrita |
| Dois produtores na mesma faixa | P1 e P2 escrevem a mesma faixa antes do fetch; um deles depois da espera | a versão escolhida é a definida no item 3 da 4.2 |
| Argumentos pequenos em página nova (caso real: Hyrule Warriors, Lichii 9e33027) | compute escreve 4 palavras de argumentos em memória nunca escrita pela GPU; a entrada que as lê vem logo depois | a entrada vê os argumentos sem depender de um limiar de tamanho |
| Escrita parcial de página (caso real: Lichii b13c4c1, #103) | a CPU escreve uma estrutura pequena ao lado de bytes da GPU na mesma página | os bytes da GPU sobrevivem ao upload, sem download síncrono |

### 4.2 Consumidor de command lists (só depois do diagnóstico e dos casos dirigidos; opt-in)

Só pode ser chamado de exato depois dos casos da 4.1.4.

**Versão de uma entrada (definição):** o snapshot da submissão com as faixas escritas por todo produtor
enviado ao ring **antes** da entrada, e só por eles. Produtores enviados entre a espera de host e a entrada
entram (na ordem do fluxo eles vêm antes); no hardware a visibilidade deles sem espera é indefinida, e
esta é uma escolha, documentada. **Revisão 3:** com essa definição, a regra do item 2 abaixo não é exata
(contraexemplo `producer_after_wait_new_page` do harness, seção 11).

**Protocolo:**

1. **Filtro:** bitmap de 1 bit por 4 KiB (2 MiB), marcado pela thread de trás quando executa uma escrita da
   GPU. **Bit desligado não dispensa espera por si só:** só prova que nenhum produtor **já executado**
   escreveu ali.
2. **Dependências, não posições (revisão 2).** "A thread de trás processou o método de espera" não é "a
   dependência foi satisfeita":
   - Acquire de semáforo: já é satisfeito hoje (a frente drena e confere o valor).
   - Acquire de syncpoint: **não é implementado** (`WaitFence` comentado, `puller.cpp:126-132`; só agenda
     `ReleaseFences`). Para servir de dependência, precisa virar uma espera real pela condição
     (syncpoint ≥ valor). **Três coisas que não podem ser confundidas (revisão 3, item 9 da seção 11):**
     1. **O valor observado.** O guest sobe primeiro (`IncrementGuest`, `fence_manager.h:143`) e é o que o
        jogo, o decodificador e as ações do composite veem (`memory_manager.cpp:281-285`,
        `native_video_decoder.cpp:127,603`, `gpu.cpp:332-335`). O host (`IncrementHost`) é o que o evento
        de espera do nvhost usa (`nvhost_ctrl.cpp:152,202`).
     2. **O processamento do produtor pela thread de trás.** A instrução de incremento roda na trás, na
        ordem do fluxo (`puller.cpp:133-138`): quando o incremento é processado, tudo o que veio antes dele
        já foi gravado.
     3. **A conclusão real do trabalho na GPU.** Só aqui o trabalho terminou.
     **Quando o host sobe (verificado):** `IncrementHost` é o callback de `SignalFence`
     (`fence_manager.h:144-145`). Com o comportamento de fence `Default` e o nível Fast, o callback roda **na
     sinalização**, logo depois de `QueueFence` (`:128-130`): o host sobe antes da conclusão. Só com fences
     adiadas (nível High, ou `GPU Fence Behavior` `Balanced` ou `Accurate`, `:113`) o callback espera a
     fence completar (`:123-125`, `:216-239`) e o host sobe na conclusão. **Um acquire satisfeito pelo valor
     do host prova ordem (2), não conclusão (3).** Riscos: incrementos vindos de outros clientes do host1x,
     valores que nunca chegam e custo no FE. Ao estourar o limite de tempo, a dependência continua não
     satisfeita: o canal fica suspenso com diagnóstico (`docs/architecture-plan.md`, 12.2 c); seguir sem ela
     é opção explícita de compatibilidade.
   - WFI: grava um evento no fluxo do Vulkan (`vk_rasterizer.cpp:1125-1145`); serve de marco de ordem, não
     de conclusão.
   - Bit de sync da entrada: drena só com "Sync Memory Operations"; sem isso, é só marco de ordem.
   Com isso, a regra é: na primeira entrada depois de uma dependência nova, a frente espera **a dependência
   ser satisfeita** e a thread de trás passar dela, e só então consulta o bitmap.
3. **Cópia ordenada na posição da entrada:** o pedido de cópia é enviado ao ring junto com a entrada,
   então fica depois de todo produtor enviado antes e antes de qualquer outro. A thread de trás grava a
   cópia (GPU → buffer da entrada, com barreira) nessa posição; a frente lê depois da fence da cópia. Custo:
   uma ida e volta (thread de trás + GPU) por entrada **candidata**. Cópias preservadas na hora do produtor
   (como o 8f1435f do Lichii) são uma otimização posterior, com o mesmo critério de versão.
   **Regra (revisão 3): a transferência tem conclusão própria.** A conclusão é a fence da cópia ordenada
   depois do produtor, e a garantia de que os bytes estão prontos **não pode depender da política de
   callbacks antecipados** (nem do valor do syncpoint, nem do modo de fence). O syncpoint dá a ordem (o
   produtor está antes da cópia no fluxo); a fence da cópia dá a conclusão. Para um **produtor de outro
   canal**, o protocolo precisa **identificar qual trabalho satisfaz a dependência** (a instrução de
   incremento ou o release daquele canal) e garantir que esse trabalho está **antes da cópia no fluxo**;
   esperar a fence da cópia **não resolve uma dependência ausente**, porque a cópia fica na posição da
   entrada, e um produtor depois dessa posição não está nela (`other_channel_missing_dependency`).
   **Política de fence:** mudar as fences globalmente para `Accurate` **não é a solução**: não cria a
   dependência que falta, e muda o custo de todos os jogos. Qualquer mudança de política de fence é
   **medida à parte no FE** (orçamento da seção 12.1 do plano de arquitetura), não decidida aqui.
4. **Publicação:** buffer próprio da entrada, release/acquire. A frente sobrepõe as faixas ao snapshot.
5. **Entradas retomadas** seguem a mesma regra, em vez da memória atual sem flush. Muda um caminho
   existente: atrás da chave, com contador.
6. **Reuso pela CPU (revisão 2: não é necessariamente corrida do jogo).** Em Fast, o emulador sinaliza
   conclusões antes da GPU emulada chegar nelas: `SignalFenceCoalesced` roda o callback na hora
   (`fence_manager.h:80-96`) e o syncpoint que o jogo vê sobe no `IncrementGuest`. O jogo pode então
   reutilizar, legalmente do ponto de vista dele, a memória de uma lista que o emulador ainda vai executar.
   Para faixas candidatas, uma escrita da CPU nessa situação não pode chegar ao buffer do host antes de a
   entrada consumidora ser executada. Duas saídas, a decidir com o caso dirigido "reuso pela CPU": segurar
   essas escritas (o mecanismo de escritas seguradas já existe) até a entrada passar, ou preservar a saída
   do produtor no momento em que ele escreve.

**Regra de visibilidade (revisão 3).** O item 2 decide **quando** a frente pode confiar no filtro. Há duas
implementações, trocáveis num ponto só (`VisibilityRule` em `tools/gpu-contract-harness/contracts.h`):

- **`exact-drain` (regra exata, padrão):** enquanto houver produtor na fila (enviado e ainda não gravado
  pela thread de trás), a entrada espera a thread de trás chegar até ela; só então consulta o filtro. No
  harness passa em todos os casos e escalonamentos (**medido no modelo**, seção 11). É cara no FE só quando há
  produtor na fila no momento do fetch; esse custo ainda **não foi medido** no jogo e entra no orçamento de
  `docs/architecture-plan.md` 12.1.
- **`after-satisfied-dependency` (otimização, desligada por padrão):** espera a dependência nova ser
  satisfeita e a thread de trás passar dela, e depois confia no filtro. Só é exata com uma **garantia
  adicional sobre os produtores**: todo produtor que uma entrada consome passou por uma dependência
  satisfeita **e** não há produtor numa página ainda não marcada entre essa dependência e o fetch. O harness
  tem dois contraexemplos (seção 11): `no_dependency` (nada entre produtor e fetch) e
  `producer_after_wait_new_page` (produtor depois da espera, em página nova, que a definição de versão
  inclui). Só pode ser ligada por escolha explícita, se o diagnóstico (4.1.2, escritas perdidas) e mais
  jogos mostrarem que a garantia vale, e mesmo assim com o verificador por amostragem.

**Riscos:** produtores fora do buffer cache não marcam o bitmap (inventário da seção 7, item 3). Bits nunca
limpos geram falsos positivos; medir contra entradas com bytes realmente diferentes. A espera real do
acquire de syncpoint muda o comportamento de todos os jogos que usam syncpoint, e por isso entra atrás de
chave própria, com A/B no FE.

**Chave e padrão:** `EDEN_GPU_LISTS=0/1`, padrão 0 até passar nos casos dirigidos e no Bananza. Depois
disso, `exact-drain` é a referência correta e o candidato a padrão (`docs/architecture-plan.md` 12.3), mesmo
com um custo pequeno no FE dentro do orçamento (12.1). Acima do orçamento, a pendência é registrada e as
opções de compatibilidade ficam explícitas e opt-in; `submitted` não volta como padrão.
`after-satisfied-dependency` não é candidata a padrão enquanto a garantia acima não estiver demonstrada.

### 4.3 Releitura de macro

Fica como está (`EDEN_NX2_MACRO_REFRESH=0` restaura as macros na frente) até o consumidor da 4.2 provar
que cobre os parâmetros de macro, incluindo parâmetros que atravessam entradas e retomadas depois de
acquire. Depois disso, o 6d3dd9a sai e a releitura volta ao Accurate e ao bit 1. O override de diagnóstico
continua.

### 4.4 Correções pequenas

15126fb (seção 3.2) com teste próprio. #256 (seção 3.3) só com ganho medido.

### 4.5 Medir a rede DLSS do FE

Seção 3.4.

## 5. Ordem e aceite

1. Matriz 4.1.1 (usuário) e, no mesmo build, FE na praça com `EDEN_NX2_MACRO_REFRESH=0` e `=1`.
2. Build de diagnóstico 4.1.2 (código), no Bananza e no FE.
3. Teste e port do 15126fb (independente).
4. Harness com os casos dirigidos da 4.1.4. **Feito no modelo** (`tools/gpu-contract-harness/`, seção 11);
   ainda falta o harness com `DmaPusher` e puller reais.
5. Espera real do acquire de syncpoint, atrás de chave, com A/B no FE.
6. Consumidor 4.2, opt-in. Aceite: casos dirigidos passando; chão certo no Bananza em Fast; no FE, draws/s
   da praça e do palácio ≥ os de antes do 6d3dd9a, e o custo fixo medido (marcações, consultas, esperas).
7. Só então: reverter o 6d3dd9a e decidir o padrão do consumidor.

## 6. O que não fazer

- Ligar correções por título.
- Executar comandos possivelmente velhos como política padrão.
- Ler a memória atual de uma lista sem saber qual versão ela representa.
- Dispensar uma espera porque um bit está desligado, sem antes satisfazer as dependências.
- Tratar "a thread de trás processou o método" como "a dependência foi satisfeita".
- Consultar ou fazer flush dos caches na frente depois de uma espera parcial.
- Portar em bloco dos forks.

## 7. Perguntas abertas (revisão 2)

1. A regra "produtor só é visível ao fetch depois de uma dependência satisfeita" vale para o NVN? O e3d8e9c
   é pista, não garantia do hardware. O diagnóstico (escritas perdidas) e os casos dirigidos respondem
   parte; o resto depende de mais jogos.
2. Para o reuso pela CPU (4.2, item 6): segurar as escritas da CPU ou preservar a saída do produtor? A
   primeira reaproveita um mecanismo existente; a segunda custa uma cópia por produtor em página candidata.
   **Revisão 3:** no modelo, segurar o upload das escritas sobre entradas submetidas e não buscadas basta
   (`cpu_reuse`), e sem isso o caso falha (`cpu_reuse_no_hold`). Falta conferir o mecanismo real de escritas
   seguradas (junção por página, limites) contra isso.
3. Inventário de produtores: storage buffers de draws e compute, imagens, storage images e render targets
   com alias, queries, escritas de CPU e HLE, cópias, mudanças de mapeamento. Quais podem produzir comandos?
4. O caminho de entradas retomadas (`UnsafeRead`) já causa problemas hoje? O diagnóstico conta.
5. A espera real do acquire de syncpoint é viável sem travar jogos que esperam incrementos de outros
   clientes do host1x (decodificador de vídeo, VIC)?
6. Os forks desenham o chão do Bananza em Fast? Ajuda, mas não confirma.

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

## 10. Revisão 2: o que mudou e por quê

| # | Ponto da revisão | Conferido | Mudança |
| --- | --- | --- | --- |
| — | Primeira leitura da CPU no Vulkan | A revisão aceitou a contestação | Sem mudança |
| 1 | Passar pelo acquire de syncpoint não satisfaz o acquire; WFI só grava um evento | Sim, `puller.cpp:126-132` (`WaitFence` comentado), `vk_rasterizer.cpp:1125-1145`. Além disso, o syncpoint que o jogo vê sobe antes da GPU (`fence_manager.h:142-146`) | 4.2, item 2: dependências satisfeitas; espera real do acquire de syncpoint como passo próprio (5.5) |
| 2 | O fallback "esperar em toda entrada que cai no bitmap" perde a primeira escrita | Sim (contraexemplo correto) | Fallback reescrito: a única opção exata sem a regra é esperar em toda entrada com produtor na fila; só opt-in |
| 3 | `gpu_modified_ranges` não guarda versões; a cópia pode pegar uma versão posterior; reuso pela CPU não é necessariamente corrida do jogo | Sim; o reuso legal vem das conclusões sinalizadas antes da hora (`fence_manager.h:80-96`) | Versão definida pela posição da entrada no ring, com a cópia ordenada ali (item 3); reuso pela CPU como item 6, com duas saídas a decidir |
| 4 | O diagnóstico tem pontos cegos e não pode confirmar o que a 4.1.3 prometia | Sim | Diagnóstico cobre entradas retomadas, conta escritas perdidas e declara o que não cobre; 4.1.3 rebaixada; casos dirigidos (4.1.4) |
| 5 | "~0 entradas filtradas ⇒ grátis no FE" | Sim | Frase trocada pela da revisão |

## 11. Revisão 3: harness dos contratos (T1) e revisão dele

Fonte: `tools/gpu-contract-harness/` (modelo com frente, trás e GPU falsos; `run.sh` com `-O2`, TSan e
ASan+UBSan). Tudo aqui é **medido no modelo**, não no emulador; o que o modelo não cobre está no `README.md`
dele.

| # | Ponto | Conferido | Mudança |
| --- | --- | --- | --- |
| 1 | A regra "produtor visível só depois de dependência satisfeita" é exata? | Não. `no_dependency` e `producer_after_wait_new_page` falham com ela (`WordMismatch`, no aleatório e em todos os forçados menos o `eager`, onde não há janela). O segundo mostra que a regra contradiz a própria definição de versão: um produtor entre a espera e o fetch, em página nova, entra na definição e fica fora do filtro | 4.2: `exact-drain` é a regra exata e o padrão; `after-satisfied-dependency` vira otimização condicionada a uma garantia adicional sobre os produtores, desligada por padrão |
| 2 | Segurar escritas da CPU sobre entradas submetidas e não buscadas | Necessário: `cpu_reuse_no_hold` falha (`WordMismatch`) no aleatório, `gpu-lags` e `eager`, e **passa** em `back-lags` (o upload vem antes do produtor, que escreve por cima). Um escalonamento só não basta para pegar | Pergunta 7.2 respondida no modelo; o mecanismo real precisa ser conferido |
| 3 | Upload das escritas da CPU por palavra (faixas exatas), não por página | Necessário: `partial_page_write_page_upload` falha sempre (`WordMismatch`); o upload da página inteira apaga as palavras da GPU (o caso do Lichii b13c4c1) | Requisito do consumidor e do mecanismo de upload |
| 4 | `valid_until` conferido antes de reaproveitar uma transferência (inclui época do mapeamento) | Necessário: `remap_ignore_valid_until` falha sempre (`WordMismatch`) | Requisito do consumidor |
| 5 | Acquire de syncpoint satisfeito pelo valor do host (`IncrementHost`), não pelo do guest | Necessário: `other_channel_guest_value` falha (`DependencyViolated`) em todos os escalonamentos menos `eager`: a entrada começa antes de a GPU concluir. As palavras ainda saem certas, porque a cópia é ordenada; o que se viola é a dependência | Reforça 4.2, item 2, e o passo 5.5 |
| 6 | Bug do cache (revisão do T1): a validade cobria só as faixas copiadas | Sim. Uma transferência vazia (o filtro não achou nada) ficava válida para sempre, e uma entrada em duas páginas não perdia a validade quando um produtor escrevia na página que o filtro tinha descartado. Com `exact-drain`, `stale_empty_transfer` e `stale_two_pages` falhavam (20/20 no `eager`, 28–37% no aleatório) | `Transfer` separa `requested` (o que foi copiado) de `covered` (a região inteira da entrada no fetch); `valid_until` cobre `covered`, e o contrato lê a validade antes do filtro. Os dois casos passam em todos os escalonamentos |
| 7 | Negativos só diziam "falhou", sem dizer por quê | Sim | Negativos tipados: cada um declara o tipo de falha (`WordMismatch`, `DependencyViolated`...), e qualquer outro tipo reprova o teste |
| 8 | (revisão do T1) Regra por dependências retirada da preferência de padrões; exact-drain como referência; custo não torna exato um caminho potencialmente incorreto | Decisão do usuário e do revisor, sobre o resultado do T1 (`no_dependency`, `producer_after_wait_new_page`) | 4.2 (chave e padrão) e `docs/architecture-plan.md` 12.3 e 13.2 alinhados |
| 9 | (revisão do T1) O valor host do syncpoint não sobe na conclusão da GPU em Fast com fences `Default`: o item 2 da 4.2 dizia que sobe na conclusão (`fence_manager.h:142-146`) e confundia valor observado, processamento pela trás e conclusão | Sim, `fence_manager.h:123-130` (callback na sinalização) e `:113` (adiado só em High, `Balanced` ou `Accurate`). Nos modos adiados o callback espera a fence (`:123-125`) | 4.2, itens 2 e 3: as três coisas separadas; um acquire pelo host prova ordem, não conclusão; a transferência tem conclusão própria (a fence da cópia); produtor de outro canal exige dependência identificada; política de fence medida à parte. Harness: dois modos de incremento do host (`host-early`, `host-deferred`), todos os casos nos dois |
| 10 | O harness não tinha o produtor de outro canal sem dependência nem o syncpoint que chega antes da GPU | Sim. `syncpoint_before_gpu` (passa nos dois modos: a entrada espera a fence da própria cópia), `syncpoint_trusted_as_ready` (negativo: falha com `WordMismatch` em `host-early`, passa em `host-deferred`) e `other_channel_missing_dependency` (falha com `WordMismatch` com as duas regras: esperar a cópia não substitui a dependência) | Requisito do consumidor: não ler bytes por causa de um syncpoint; conclusão pela fence da cópia |
| 11 | O harness não tinha downloads da GPU para a memória do jogo (Lichii 3165a2b e 15126fb) | Sim. `pending_download_vs_cpu_write` passa (escrita nova sobrevive; o upload seguinte leva os bytes novos) em ordens em que o download conclui antes e depois da escrita. Negativos: sem cancelar os pendentes antes da escrita da CPU (`pending_download_no_cancel`, `WordMismatch`); com um conjunto único de faixas pendentes (`pending_download_single_set`, `WordMismatch`) | Requisitos do mecanismo de downloads: cancelar o pendente na escrita da CPU (`ClearDownload`, `buffer_cache.h:209-216`) e um conjunto por download. Fora do modelo: a janela da escrita **segurada** (README do harness) |
