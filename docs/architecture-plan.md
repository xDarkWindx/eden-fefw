# Plano de arquitetura: um núcleo que não muda por jogo

Escrito em 2026-11-05 sobre `3c5c04a`. Documento para revisão crítica; as perguntas abertas estão na seção 9.
Graus de certeza como em `docs/gpu-produced-data-plan.md`: **verificado** (lido no código daqui),
**medido** (número de um commit ou doc, daqui ou dos forks), **hipótese**.

**Revisão 1 (2026-11-05):** incorpora a revisão estática sobre `af61d42`, conferida no código. O que mudou
está na seção 10. As mudanças centrais: a tabela de páginas deixou de ser autoridade de coerência e virou
filtro; o modo "aprendido" não vale para comandos; nenhuma consulta a caches sai da thread de trás; e a
ordem de implementação põe diagnóstico e protocolo antes da arquitetura global.

## 0. Objetivo

Hoje, cada jogo novo pode exigir mudanças no núcleo do emulador (coerência de memória, sincronização,
ligação de recursos), além do esperado (instruções de shader, macros, serviços). O objetivo é que **um jogo
novo só peça trabalho na camada de ISA**: instruções e formas de shader, métodos de engine e comandos de
serviço. O que o jogo faz com memória, ordem e recursos deve ter um caminho geral no núcleo, ligado pelo
padrão que o jogo usa, e não pelo título.

Três requisitos, nas palavras do usuário:

- **Maleável:** um comportamento novo entra como uma política ou um consumidor novo de uma estrutura que já
  existe, não como uma estrutura nova.
- **Idempotente:** as operações do núcleo são do tipo "garanta que X esteja em dia". Repetir uma delas tem
  o mesmo efeito que fazê-la uma vez. A consulta repetida custa pouco, mas não zero. Nenhuma correção depende
  de um evento ser entregue exatamente uma vez.
- **Rápida, mas segura antes de rápida (diretriz do usuário, 2026-11-05):** uma pequena regressão no FE é
  aceitável em troca de algo mais seguro, adaptável e agnóstico. O mais importante é antecipar e se proteger
  dos problemas antes que eles aconteçam, porque eles vão acontecer. O desempenho continua medido em todo
  passo, contra um **orçamento** explícito (seção 12.1), e não contra "zero regressão".

## 1. De onde vem a troca constante (verificado neste fork e nos forks)

| Origem | Exemplos | Natureza |
| --- | --- | --- |
| Instruções e formas de shader | série `lower_to_maxwell` (8543dea, 89e0c53, 61eb9e1, 653d141...), geometry (0343eab), IABS (6e13f7d) | **Esperada.** É a camada que deve crescer. |
| Ponteiros que o recompilador não consegue mapear para buffers | d749943 (ponteiros carregados x slots NVN), atômicos de contadores de append do Bananza no Lichii | **Arquitetura.** A memória global escolhe, entre os buffers rastreados, o de base mais próxima abaixo do endereço (`spirv_emit_context.cpp:949-976`). Sem correspondência, loads podem dar zero e stores não escrevem; com rastreio errado, o acesso pode ir ao buffer errado. |
| Dados que a GPU produz e a CPU do emulador consome | 6d3dd9a e c2a5438 (macros), command lists (`docs/gpu-produced-data-plan.md`), downloads em fences (Lichii 15126fb) | **Arquitetura.** Cada consumidor tem sua regra; a precisão é uma chave global. |
| Ordem entre as threads | nove categorias de `Drain` (`DrainSite`, `back_thread_stats.h:248`, incluindo `DrainOther`; não são nove pontos de chamada); esperas por entrada nos forks (9479d8f) | **Arquitetura.** |
| Trabalho de GPU feito na CPU ou trocado por substituto | reset indireto do Bananza na CPU (98b6de5), substitutos de tensor e DLSS nos forks | **Arquitetura**, por falta de um lugar próprio. |
| Ligação de texturas por draw | bancos, tabelas compactas, memos (100adbf, 20d998e, 9c1e70d) | **Desempenho**, e frágil: cada jogo com outro padrão de banco pede outro atalho. |
| Variáveis de ambiente | 19 nomes distintos em chamadas `getenv("EDEN_...")` em `src/` (grep; não conta nomes montados em tempo de execução nem configurações da interface): `EDEN_BACK_THREAD_STATS`, `EDEN_CPU_WRITE_PAGES`, `EDEN_FLUSH_DRAWS`, `EDEN_GPU_HIGH_PARTS`, `EDEN_GPU_SPLIT`, `EDEN_HELD_WRITE_MERGE`, `EDEN_INLINE_DEFER`, `EDEN_MME_JIT`, `EDEN_NATIVE_ARITHMETIC_REPORT`, `EDEN_NATIVE_DESCRIPTOR_REPORT`, `EDEN_NATIVE_FRONTEND`, `EDEN_NATIVE_SHADER_FIXTURES`, `EDEN_NATIVE_SPIRV_CHECK`, `EDEN_NO_HEAP_REUSE`, `EDEN_NX2_MACRO_REFRESH`, `EDEN_NX2_RESETFIX`, `EDEN_NX2_VDEC_EOS_RELEASE_NOW`, `EDEN_TEXTURE_TABLE_STATS`, `EDEN_WINDOW_TITLE` | **Operação.** |

## 2. Princípios

1. **Paga quem usa.** Cada mecanismo tem um teste barato que decide se o caminho caro roda. "Barato" é
   medido no FE; uma leitura atômica não é custo zero, ainda mais quando roda por página ou por draw.
2. **Versões ligadas a operações, não a páginas soltas.** Quem afirma "estes bytes estão em dia" é a
   operação que os transferiu (que faixas, de que versão, concluída quando), e não um campo de página.
3. **Uma fonte de verdade por fato, com dono.** Os caches são donos do conteúdo e das faixas exatas, e
   moram na thread de trás. Qualquer outra thread recebe resultados publicados por ela, nunca consulta os
   caches.
4. **Falha fechada e visível.** Instrução ou forma não suportada vira rejeição contada, com dump do
   programa, e nunca lixo silencioso nem crash.
5. **Políticas são dados.** Cada escolha de coerência ou de ordem é uma política registrada, com nome,
   padrão, override uniforme e contador.
6. **Sem especulação em comandos.** Dados lidos pela CPU do jogo podem usar políticas aprendidas;
   comandos (listas, parâmetros de macro, argumentos indiretos) só usam modos exatos, porque um comando
   errado muda estado que persiste.
7. **Código específico de jogo só como plugin**, reconhecido pela identidade do programa, fora do núcleo,
   desligável e com log.
8. **Verificável fora do jogo.** Toda mudança de núcleo deve poder ser checada no corpus de shaders e,
   depois, em replays de frames.
9. **Seguro por padrão, rápido por medição.** Entre dois modos corretos, o padrão é o mais barato. Entre um
   modo correto e um rápido que pode errar, o padrão é o correto, se couber no orçamento. Modos que podem
   errar só por escolha explícita.
10. **Mudança automática só para o lado seguro.** O emulador pode, sozinho, passar um consumidor para um modo
    mais exato quando detecta um problema; nunca o contrário dentro da sessão.
11. **Defender no ponto de consumo.** Todo dado que vem de um produtor que pode ter falhado ou atrasado
    (contagens de draw, argumentos indiretos, tamanhos, ponteiros) é conferido onde é usado, e um valor
    impossível vira trabalho pulado e contado, não crash nem device lost.

## 3. Camadas

### 3.1 L0 — ISA (a única que cresce por jogo)

- **Hoje:** decodificação Ampere → lowering para Maxwell → IR → SPIR-V. Rejeições contadas, com dumps
  (0b08912). `src/tests/video_core/native_shader.cpp` tem 292 declarações `TEST_CASE` (não é medida de
  cobertura da ISA, e elas não foram executadas para este plano).
- **Falta:** compilar um corpus de dumps inteiro fora do emulador e dizer o que regrediu. O Lichii tem
  `tools/shader-sweep` (ambientes serializados do cache de pipelines, sem Vulkan, `spirv-val` em cima).
- **Passo:** portar a ideia, com corpus por jogo fora do repositório. Roda na nuvem. Fluxo de um jogo novo:
  dump → sweep → implementar a instrução → fixture → sweep sem regressão.
- **Tensor (HMMA):** `VK_KHR_cooperative_matrix` como caminho rápido onde houver suporte. **Hipótese** de
  desempenho a medir.

### 3.2 L1 — Memória: filtro de páginas + autoridade nos caches

- **Filtro:** um bitmap de 1 bit por 4 KiB do espaço de dispositivo (2 MiB), "esta página tem ou teve
  faixas escritas pela GPU entre os produtores já executados". Marcado pela thread de trás. Os
  consumidores usam o filtro para pular rápido o caso comum. **Bit desligado** só prova que nenhum produtor
  **já executado** escreveu ali; produtores na fila são tratados pela ordem (seção 3.3).
- **Autoridade:** continua nos caches, que guardam as faixas exatas (`gpu_modified_ranges`,
  `cpu_read_ranges`, Preflushable). Quem precisa dos bytes pede à thread de trás uma **transferência**:
  um objeto com as faixas, a posição do produtor no ring, a fence de conclusão na GPU e um buffer de destino.
  O consumidor só usa os bytes depois da conclusão e da publicação (release/acquire). Uma transferência
  parcial nunca marca a página inteira como em dia.
- **Custos que a revisão apontou:** marcar o filtro acontece onde a GPU escreve, o que inclui o caminho de
  draws (`MarkWrittenBuffer` com storage escrito). A marcação é uma leitura e, só se o bit estiver
  desligado, um `fetch_or` (como no Eglantine). Mesmo assim é custo por draw com escrita: medir no FE antes.
- **Ainda em aberto:** identidade do mapeamento (remapeamento do espaço da GPU invalida bits e
  transferências), escritas da CPU depois de uma transferência e páginas reaproveitadas (falsos positivos
  dos bits que nunca são limpos).
- **Primeiro consumidor:** command lists (`docs/gpu-produced-data-plan.md`, 4.2).

### 3.3 L2 — Ordem

- **Hoje:** o ring do split já é uma linha do tempo (posição escrita pela frente, `consumed` pela de trás).
  Cada local que precisa de algo da de trás chama `Drain` e espera a fila inteira.
- **Proposta:** marcos anotados na frente ao enviar (último produtor de compute ou cópia, última espera de
  host) e um primitivo `WaitFor(posição)` que espera a de trás passar de um marco.
- **Posição não é dependência (revisão 2):** a thread de trás ter processado um método de espera não quer
  dizer que a condição dele foi satisfeita. Hoje o acquire de syncpoint nem espera (`puller.cpp:126-132`), e
  um produtor de outro canal pode nem estar no ring ainda. Um marco de espera de host carrega a condição
  (semáforo e valor, syncpoint e valor) e só vale quando ela é satisfeita; passar da posição serve para
  ordem, não para conclusão. Conclusão na GPU é outra coisa ainda (fence).
- **Regra de dono (revisão):** depois de um `WaitFor`, a thread de trás **continua** executando. Por isso a
  frente não pode consultar `IsMemoryDirty`, fazer `FlushRegion` nem tocar no scheduler. Hoje essas consultas
  passam pelo rasterizador e pelos locks dos caches (`memory_manager.cpp:518`, `vk_rasterizer.cpp:862-878`).
  O padrão é: a frente envia uma chamada para a de trás (`Defer` com resultado), a de trás executa na
  posição certa e publica o resultado, e a frente espera só por ele. Isso vale também para o tempo de vida
  das cópias.
- **Cuidado (medido aqui, 3.14 do plano do split):** cada espera que esvazia o ring deixa a thread de trás
  ociosa até a frente voltar. Trocar `Drain` é feito local a local, cada um com A/B no FE.
- **O que não muda:** o dono do scheduler e a ordem dos chunks.

### 3.4 L3 — Recursos

**Memória global:**

- **Hoje (verificado):** escolha do buffer rastreado de base mais próxima abaixo do endereço
  (`spirv_emit_context.cpp:976`); sem correspondência, loads podem dar zero e stores não escrevem. Atômicos
  globais de 64 bits lançam exceção nos emissores citados (`emit_spirv_atomic.cpp:473-485`).
- **BDA (`VK_KHR_buffer_device_address`) é um projeto, não um fallback isolado (revisão):** exige residência
  de toda página que o shader pode tocar, endereços que continuam válidos quando o buffer cache substitui
  buffers, aliasing e atomicidade corretos entre acessos rastreados e por ponteiro, limites e remapeamentos,
  e coerência (o emulador não sabe o que o shader escreveu por ponteiro). Fica como estudo separado.
- **Alternativa menor para os atômicos de append:** ampliar a identificação e o binding dos buffers de
  contador (o `CollectAtomicPointers` do Lichii), sustentada pelos shaders capturados.

**Texturas:** heap bindless persistente com slots versionados (TIC escrito, imagem criada, apagada,
reescalada ou escrita pela CPU sobem a versão). É o maior ganho possível no FE, e o mais arriscado. Antes
dele, é preciso resolver aliases, preparação das imagens e versões dos descritores. O degrau de entrada são
os arrays de ids (Eglantine #256), só com ganho medido.

### 3.5 L4 — Políticas

Uma tabela única (`video_core/policy.h`), lida uma vez no início, com sintaxe uniforme
(`EDEN_POLICY=nome=modo,...`). As variáveis atuais viram aliases durante uma transição.

| Política | Modos | Padrão |
| --- | --- | --- |
| `list_fetch` | `submitted` / `exact` | `submitted` até a validação (gpu-produced-data-plan 4.2) |
| `macro_params` | `submitted` / `exact` | como hoje |
| `cpu_buffer_read` | `none` / `learned` / `fence` | `learned` (o que existe hoje no Vulkan) |
| `cpu_texture_read` | `none` / `exact` | `none` (hoje) |
| `semaphore_order` | `immediate` / `ordered` | `immediate` |
| `invalidate_flush` | `off` / `on` | `off` |
| `split` | `0` / `1` / `2` | `2` |

- **"Learned"** só para dados lidos pela CPU do jogo (princípio 6), nunca para comandos.
- **Accurate não é "todas as políticas no máximo" (revisão).** O nível High hoje controla, além das quatro
  partes de `accuracy_parts.h`: queries precisas (`vk_query_cache.cpp:177`), renderização condicional
  (`:1520-1540`), o split desligado (`gpu_split.cpp:77-80`), o adiamento de fences (`fence_manager.h:82`,
  `:113`) e o DMA Safe por padrão (`dma_pusher.cpp:142-143`). Primeiro mapear todos os efeitos; depois
  definir um preset `accurate` que os reproduza, e só então aposentar `EDEN_GPU_HIGH_PARTS`.
- Durante a migração, nenhuma política muda de padrão: o passo é só reorganizar e medir.
- Cada política tem contador no `EDEN_BACK_THREAD_STATS`.

### 3.6 L5 — Plugins por identidade

- Interface única: `match(program_hash, contexto) → handler`. Custo: uma busca de hash por programa novo,
  não por draw. Cada plugin com chave para desligar, log na primeira ação e teste com o programa capturado.
- Primeiro: o reset indireto do Bananza (98b6de5), sem mudar de comportamento.

### 3.7 L6 — Verificação e observabilidade

- **Corpus de shaders** (seção 3.1).
- **Replay de frames (longo prazo, revisão):** GPFIFO mais a memória lida não basta quando o conteúdo atual
  existe só em recursos da GPU. Precisa capturar estado inicial coerente, mapeamentos, estado das engines,
  sincronizações e os dados produzidos dentro do frame. Lavapipe serve para validação funcional parcial,
  não para desempenho nem para igualdade numérica com a NVIDIA. A comparação é por pixel, com métrica
  definida (por exemplo, erro máximo por canal e fração de pixels acima do limite).
- **Depuração ao vivo:** o servidor opt-in com MCP do Lichii (#111), quando o fluxo pedir.

## 4. Como fica a vida com um jogo novo

1. Rodar, juntar o log e os dumps.
2. Rejeições de shader → L0: implementar, fixture, sweep.
3. Imagem errada sem rejeição → diagnóstico do consumidor suspeito (o build de diagnóstico de listas é o
   modelo), e depois políticas em modo exato, uma por vez, com o resto constante.
4. Se nenhuma política consertar, há um buraco de arquitetura: vira um consumidor novo do filtro e das
   transferências, ou uma política nova, não um mecanismo novo.
5. Algo que só o jogo faz e que não vale emular → plugin.

## 5. Ordem de implementação (revisada)

Cada passo entra sozinho, com A/B no FE (praça e palácio, draws/s com estatísticas desligadas) e sem mudar
o padrão antes de medir.

| # | Passo | Risco para o FE |
| --- | --- | --- |
| 1 | Corpus de shaders + sweep; inventário de configurações e de todos os efeitos do nível High | nenhum |
| 2 | Diagnóstico de command lists e matriz de testes (gpu-produced-data-plan 4.1), sem trocar a fonte | nenhum com o diagnóstico desligado |
| 3 | Teste das faixas de downloads pendentes (15126fb), independente do Bananza | baixo |
| 4 | Casos dirigidos num harness (primeira escrita, outro canal, retomada, reuso pela CPU, dois produtores); espera real do acquire de syncpoint, atrás de chave | médio na espera de syncpoint; A/B |
| 4b | Protocolo mínimo: filtro, dependências satisfeitas (não só posições), cópias ordenadas na posição do consumidor e publicadas pela thread de trás, conclusão na GPU | médio; medir o custo fixo e das esperas |
| 5 | Consumidor de command lists, opt-in; validar reutilização pela CPU, primeira escrita e retomada depois de acquire | baixo enquanto opt-in |
| 6 | Decidir: reverter o 6d3dd9a no split, padrão do consumidor | o objetivo é ganhar de volta as macros na frente |
| 7 | Registro de políticas (sem mudar padrões) | nenhum |
| 1b | Proteções da seção 12.2 que não dependem de nada: guardas no consumo, contenção de exceções, esperas com limite, relatório de saúde | baixo; entram cedo, junto do passo 1 |
| 8 | Trabalhos separados, cada um com evidência própria: arrays de ids, plugins, BDA, heap bindless, replay | varia |

## 6. O que não muda

- O split frente/trás e o dono do scheduler.
- As otimizações do FE (memos, bancos, tabelas compactas, escritas seguradas, `EDEN_FLUSH_DRAWS`), até que
  algo as torne desnecessárias com A/B.
- O formato dos commits e dos docs.

## 7. Riscos

- **Custo no caminho quente:** a marcação do filtro roda onde a GPU escreve, o que inclui draws. Medir no FE.
- **Esperas novas:** cada marco de espera de host vira, no pior caso, uma espera parcial a mais na frente.
  Medir quantas por segundo no FE.
- **Falsos positivos do filtro:** páginas reaproveitadas pagam o caminho caro; medir contra bytes realmente
  diferentes.
- **Migração longa:** cada passo deixa o emulador funcionando e mais rápido ou igual; não há "big bang".

## 8. Relação com os outros planos

- `docs/gpu-produced-data-plan.md`: passos 2 a 6.
- `docs/back-thread-split-plan.md`: continua valendo para o desempenho do FE; o passo 4 conversa com os
  achados dele sobre `Drain`.

## 9. Perguntas abertas (revisão 1)

1. O filtro de 1 bit basta como classificação conservadora, com as versões só nas transferências? (A
   revisão respondeu que sim para começar; fica a confirmar com os falsos positivos medidos.)
2. Que granularidade as transferências precisam: faixas exatas do cache, ou subblocos fixos?
3. Como invalidar bits e transferências quando o espaço da GPU é remapeado?
4. Que efeitos do nível High faltam no inventário do passo 1, além dos listados na seção 3.5?
5. O replay deve começar pela captura no nível do GPFIFO com estado inicial coerente, ou por um subconjunto
   (um único draw com seus recursos)?

## 10. Revisão 1: o que mudou e por quê

| Ponto da revisão | Mudança |
| --- | --- |
| Versões por página e átomos relaxados não formam protocolo (escritor na fila, conclusão na GPU, publicação, download antigo, faixas parciais) | A tabela virou filtro de 1 bit; a autoridade fica nos caches; transferências com faixas, posição do produtor, fence e publicação (3.2) |
| `WaitFor` não autoriza a frente a usar caches | Regra de dono na 3.3: consultas e flushes só na thread de trás, com resultado publicado |
| "Aprendido" para comandos aceita corrupção que persiste | Princípio 6; `list_fetch` sem modo aprendido (3.5) |
| Marcar escritas acontece no caminho de draws | Corrigido em 3.2 e 7; deixa de ser "fora do draw" |
| "Uma leitura atômica" não é custo zero; idempotência é efeito, não custo | Seção 0 e princípio 1 reescritos |
| BDA é projeto maior, não fallback | 3.4 rebaixado a estudo; alternativa menor para atômicos de append |
| Accurate não é "todas as políticas em exact" | Inventário dos efeitos do High antes de qualquer preset (3.5) |
| Replay precisa de estado coerente; lavapipe só funcional; métrica de pixels | 3.7 reescrita |
| Memória global também pode ir ao buffer errado | Seção 1 corrigida |
| Contagem de 19 variáveis sem critério | Critério e lista na seção 1 |
| Ordem: diagnóstico e protocolo antes da arquitetura global | Seção 5 reescrita conforme a revisão |
| (revisão 2) Posição no ring não é dependência satisfeita | 3.3 e passo 4 da seção 5 |
| (diretriz do usuário) Segurança antes de desempenho, com orçamento | Seção 0, princípios 9-11, seção 12, passo 1b |

## 11. Teste com casos reais: os Zeldas do Lichii

Os commits do Lichii para Hyrule Warriors: Age of Imprisonment e para "Ocarina" (o Zelda que eles chamam
de "green-hat kid adventure") servem para testar a promessa da seção 0. Classificação (lida nos commits,
não reproduzida):

| Commit | O que corrige | Camada | Com esta arquitetura |
| --- | --- | --- | --- |
| b328477, c9ebb5c (shader), e442211, fc517e3, 982cab9, a10e013 | Formas de shader, sombreamento de registradores (porta da casa do Link com NaN) | L0 | Continua por jogo, como esperado |
| b6706b0 | `WaitIfEqual64` (arbitragem tipo 3) no kernel | Serviços/kernel | Continua por jogo, como esperado |
| 9e33027 | Primeira escrita: argumentos de draw de 4 palavras em página nova, lidos antes de o compute rodar; contornado esperando escritores só em entradas de até 8 palavras | L1/L2 | Coberto pelo protocolo de dependências, sem limiar de tamanho (é o contraexemplo da revisão 2, item 2, num jogo real) |
| b13c4c1 | Escrita da CPU ao lado de bytes da GPU na mesma página; o rastreio por página perdia os bytes da GPU | L1 | Coberto pela autoridade por faixas exatas (3.2), não por página |
| 3165a2b (#103) | Desempenho da correção anterior (Ocarina: thread de trás esperando 60% do tempo) | L1 | O mesmo item: com faixas exatas, a escrita da CPU vai ao buffer sem download |
| 15126fb | Downloads pendentes da mesma faixa se confundindo (device lost) | L1 | Coberto pelas transferências com faixas e versão próprias (3.2) |
| 92a1a0f | Draws com contagens de lixo quando o compute produtor foi rejeitado | Princípio 4 | Vira regra geral: consumidor de produtor rejeitado falha fechado |

Resumo: de 12 commits, 7 são o trabalho esperado (ISA e kernel) e 5 são contornos de coerência e ordem que
esta arquitetura pretende tornar desnecessários. "Pretende": nada disso está implementado, e o protocolo
ainda tem perguntas abertas (seção 9 e `docs/gpu-produced-data-plan.md`, seção 7).

Este fork: o `WriteMemory` daqui não baixa a página (`buffer_cache.h:132-138`), então o b13c4c1 não se
aplica como está; falta verificar se o upload de páginas marcadas pela CPU sobrescreve bytes da GPU no
mesmo buffer. Esse é o caso dirigido "escrita parcial de página" do outro plano.

## 12. Proteção antecipada

A diretriz é antecipar os problemas e se proteger deles antes que aconteçam. Três tipos de proteção, da mais
forte para a mais fraca: **prevenir** (o modo correto por padrão), **conter** (um erro vira trabalho pulado e
contado, não crash) e **detectar** (o emulador percebe sozinho e escala para o modo seguro).

### 12.1 Orçamento de desempenho

Proposta, para o usuário confirmar ou mudar: cada item de segurança pode custar até **5% de draws/s** na
praça e no palácio do FE, e a soma dos itens até **10%**, sempre medido e registrado no doc do item. Acima
disso, a decisão é do usuário. Com a praça em ~21 FPS e o FPS em degraus (30/24/22/20), 5% de draws/s pode
aparecer como a perda de um degrau em parte dos frames; o número observado também é registrado.

### 12.2 Mecanismos

| # | Mecanismo | Tipo | Hoje neste fork (verificado) | Proposta |
| --- | --- | --- | --- | --- |
| a | Guardas no ponto de consumo | conter | Nenhuma guarda geral; só um `ASSERT` nas macros HLE do Switch 1 (`macro.cpp:112`) | Contagens de vértices e índices contra o tamanho dos recursos ligados, argumentos indiretos, tamanhos de alocação (o caso do `QuadIndexBuffer` do Lichii 92a1a0f): valor impossível = draw pulado, log com limite e contador por motivo |
| b | Contenção de exceções nas threads da GPU | conter | Exceções de shader são capturadas (`vk_rasterizer.cpp:392`, `vk_pipeline_cache.cpp`); o laço da thread de trás do split não captura nada | Capturar no item de trabalho (draw, dispatch, cópia): pular o item, marcar o canal como degradado, log e contador; nunca abortar o processo por um item |
| c | Esperas com limite | conter | Acquires esperam sem limite; o `acquire_timeout` é TODO (`puller.cpp:293`) | Toda espera nova (a espera real do syncpoint, por exemplo) com limite de tempo: ao estourar, log com o que esperava e política `continue` ou `hold` (padrão `continue`) |
| d | Verificador por amostragem | detectar | Não existe | Em builds normais, 1 de cada N consumidores candidatos (listas, macros, argumentos indiretos) é comparado com a versão coerente, por cópia assíncrona da GPU para um buffer privado. Diferença = log uma vez com os detalhes, escalada daquele consumidor para o modo exato até o fim da sessão (princípio 10) e linha no relatório. Custo dentro do orçamento; N ajustável |
| e | Relatório de saúde | detectar | `EDEN_BACK_THREAD_STATS` mede tempo, não saúde | Um bloco curto no log a cada 60 s e no fim da sessão: escaladas, rejeições por estágio, draws pulados por motivo, escritas perdidas, esperas estouradas, políticas em vigor. O usuário manda esse bloco em vez do log inteiro |
| f | Fuzz da entrada de comandos | prevenir | Não existe | No harness dos casos dirigidos: listas mutadas e aleatórias contra `DmaPusher` e puller reais; critério: sem crash, sem travar, sem estado corrompido depois de uma lista inválida |
| g | Modo verificação | detectar | Havia verificação das cópias de registradores do split (`EDEN_GPU_SPLIT_VERIFY` no `docs/perf-handoff.md`), removida na limpeza | `EDEN_VERIFY=1`: invariantes dos caches (faixas da GPU contra o rastreio), cópias de registradores da frente, versões das transferências; caro, só para investigação |
| h | Corpus, casos dirigidos e (depois) replay | prevenir | Testes de shader existem | Rodam na nuvem a cada mudança de núcleo, antes de chegar ao usuário |

### 12.3 O que muda nos padrões

- `list_fetch`: o padrão, depois de validado, é o modo exato mais barato que passar nos casos dirigidos, se
  couber no orçamento. Ordem de preferência: exato por dependências; exato por espera em toda entrada
  com produtor na fila (`exact-drain`); e só se os dois estourarem o orçamento, `submitted` com o
  verificador por amostragem (que detecta, mas não previne), por decisão do usuário.
- `macro_params`: mesma regra.
- `cpu_buffer_read` continua `learned`: o erro possível é um dado lido uma vez, não um comando, e o
  verificador cobre a detecção.

### 12.4 Limites

- Detectar não é prevenir: o verificador por amostragem pega um problema novo depois de algumas ocorrências.
  Por isso ele é a última linha, não a primeira.
- Guardas no consumo evitam crash e device lost, mas um draw pulado ainda é imagem errada; o relatório de
  saúde é o que transforma isso em diagnóstico.
- Conter exceções pode esconder bugs; por isso cada contenção conta, loga e aparece no relatório.
