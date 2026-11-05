# Plano de arquitetura: um núcleo que não muda por jogo

Escrito em 2026-11-05 sobre `3c5c04a`. Documento para revisão crítica; as perguntas abertas estão na seção 9.
Graus de certeza como em `docs/gpu-produced-data-plan.md`: **verificado** (lido no código daqui),
**medido** (número de um commit ou doc, daqui ou dos forks), **hipótese**.

## 0. Objetivo

Hoje, cada jogo novo pode exigir mudanças no núcleo do emulador (coerência de memória, sincronização,
ligação de recursos), além do esperado (instruções de shader, macros, serviços). O objetivo é que, depois
deste plano, **um jogo novo só peça trabalho na camada de ISA**: instruções e formas de shader, métodos de
engine e comandos de serviço. Tudo o que o jogo faz com memória, ordem e recursos já deve ter um caminho
geral no núcleo, ligado pelo padrão que o jogo usa, e não pelo título.

Três requisitos, nas palavras do usuário:

- **Maleável:** um comportamento novo entra como uma política ou um consumidor novo de uma estrutura que já
  existe, não como uma estrutura nova.
- **Idempotente:** as operações do núcleo são do tipo "garanta que X esteja em dia". Chamá-las duas vezes
  custa o mesmo que uma. Nenhuma correção depende de um evento ser entregue exatamente uma vez.
- **Rápida:** o caminho que um jogo não usa não custa nada. O benchmark é o Fortune's Weave na praça (~21 FPS
  aqui contra ~14 nos forks, medido pelo usuário), que não pode piorar em nenhum passo.

## 1. De onde vem a troca constante (verificado neste fork e nos forks)

| Origem | Exemplos | Natureza |
| --- | --- | --- |
| Instruções e formas de shader | série `lower_to_maxwell` (8543dea, 89e0c53, 61eb9e1, 653d141...), geometry (0343eab), IABS (6e13f7d) | **Esperada.** É a camada que deve crescer. |
| Ponteiros que o recompilador não consegue mapear para buffers | d749943 (ponteiros carregados x slots NVN), atômicos de contadores de append do Bananza no Lichii | **Arquitetura.** A memória global é uma busca entre os buffers rastreados (`spirv_emit_context.cpp:949-960`, verificado); um ponteiro que o rastreio não acha vira nada. |
| Dados que a GPU produz e a CPU do emulador consome | 6d3dd9a e c2a5438 (macros), listas escritas pela GPU (Eglantine 8f1435f), downloads em fences (Lichii 15126fb) | **Arquitetura.** Cada consumidor tem sua regra; a precisão é uma chave global. |
| Ordem entre as threads | nove motivos de `Drain`, cada um esperando a fila inteira (`DrainSite`, `back_thread_stats.h:248`); esperas por entrada nos forks (9479d8f) | **Arquitetura.** |
| Trabalho de GPU feito na CPU ou trocado por substituto | reset indireto do Bananza na CPU (98b6de5), substitutos de tensor e DLSS nos forks | **Arquitetura**, por falta de um lugar próprio para isso. |
| Ligação de texturas por draw | bancos, tabelas compactas, memos (100adbf, 20d998e, 9c1e70d) | **Desempenho**, e frágil: cada jogo com outro padrão de banco pede outro atalho. |
| Variáveis de ambiente | 19 `EDEN_*` lidas no código (verificado), cada uma com sua sintaxe e seu lugar | **Operação.** |

A seção 3 trata cada linha "arquitetura" com uma camada própria.

## 2. Princípios

1. **Paga quem usa.** Cada mecanismo tem um teste barato (uma leitura atômica, um bit) que decide se o
   caminho caro roda. Sem o padrão, o custo é esse teste.
2. **Versões em vez de eventos.** Página, slot de textura e região têm número de versão. "Garantir em dia"
   compara versões: é idempotente, tolera chamadas repetidas e não perde nada se uma notificação atrasar.
3. **Uma fonte de verdade por fato.** "Quem escreveu esta página por último" mora num lugar só, legível
   por qualquer thread sem lock. "Até onde a thread de trás chegou" é uma posição no ring.
4. **Falha fechada e visível.** Instrução ou forma não suportada vira rejeição contada, com dump do
   programa, e nunca lixo silencioso nem crash.
5. **Políticas são dados.** Cada escolha de coerência ou de ordem é uma política registrada, com nome,
   padrão, override uniforme e contador. "Accurate" passa a ser "todas as políticas no modo exato".
6. **Código específico de jogo só como plugin.** Ele é reconhecido pela identidade do programa (hash do shader
   ou da macro), fica fora do núcleo, pode ser desligado e loga quando age.
7. **Verificável fora do jogo.** Toda mudança de núcleo deve poder ser checada no corpus de shaders e,
   depois, em replays de frames, antes de chegar ao usuário.

## 3. Camadas

### 3.1 L0 — ISA (a única que cresce por jogo)

- **Hoje:** decodificação Ampere → lowering para Maxwell → IR → SPIR-V. Rejeições são contadas e há dumps
  (0b08912). `src/tests/video_core/native_shader.cpp` tem 292 casos (verificado).
- **Falta:** uma ferramenta que compile um corpus de dumps inteiro fora do emulador e diga o que regrediu.
  O Lichii tem `tools/shader-sweep` (compila ambientes serializados do cache de pipelines, sem Vulkan;
  `spirv-val` em cima) e fixtures de casos reais.
- **Passo:** portar a ideia do shader-sweep, com um corpus por jogo (FE, Bananza e os próximos) guardado fora
  do repositório. Dá para rodar na sessão de nuvem, sem o PC do usuário. O fluxo de um jogo novo fica:
  dump → sweep → implementar a instrução → fixture → sweep sem regressão.
- **Tensor (HMMA):** hoje é emulado por instrução (verificado: `frontend/ampere/visitor.cpp`). Um caminho
  geral e rápido seria `VK_KHR_cooperative_matrix`, onde o driver suporta. É uma **hipótese** de desempenho
  a medir. Ele substituiria os substitutos por jogo dos forks pelo mesmo cálculo, feito mais rápido.

### 3.2 L1 — Memória: tabela de estado de página

Uma estrutura única, global, legível sem lock (átomos relaxados), por página de 4 KiB do espaço de
dispositivo:

| Campo | Quem escreve | Para quê |
| --- | --- | --- |
| `gpu_version` | thread de trás, ao executar uma escrita da GPU (storage, cópia, render target, query) | Algo mais novo pode estar só no host |
| `guest_version` | quem copia para a memória do jogo (download, flush) | O que a memória do jogo já tem |
| `writer_kind` (bits) | idem | compute, draw, cópia, query: escolhe a espera certa |
| `observed` (bits) | consumidores | A CPU do jogo leu, ou uma lista ou macro leu: liga o modo exato só nessa página |

- **Consumidores**, cada um com uma função `Ensure(range)` idempotente: busca de command list, leitura de
  FIFO por macro, parâmetros de macro, acquire de semáforo, leitura da CPU do jogo (via trap), upload de
  textura e buffer. `Ensure` compara `gpu_version` com `guest_version`. Se estiverem iguais, retorna (é o
  caminho do FE). Se não, aplica a política do consumidor (seção 3.5).
- **O que já existe e vira caso particular:** `gpu_modified_ranges`, `cpu_read_ranges` e Preflushable no
  buffer cache (que guardam o conteúdo, enquanto a tabela só diz se pode estar velho), e o bitmap de
  listas do Eglantine, que é esta tabela com um bit e sem versão.
- **Custo:** 36 bits de espaço com páginas de 4 KiB são 16 Mi páginas. Com 8 bytes por página, dá 128 MiB, demais.
  Duas saídas: tabela de dois níveis alocada sob demanda (só as páginas mapeadas pela GPU), ou versões de
  32 bits + 8 bits de flags (80 MiB, ainda grande). A primeira é a recomendada; **o revisor deve pesar**.
- **Primeiro consumidor:** a busca de command lists, que é a fase A de `docs/gpu-produced-data-plan.md`. É o
  caso que já tem um bug concreto (Bananza) e um jogo que não pode pagar (FE).

### 3.3 L2 — Ordem: uma linha do tempo

- **Hoje:** o ring do split já é uma linha do tempo (posição escrita pela frente, `consumed` pela de trás).
  Mas cada local que precisa de algo da de trás chama `Drain` (espera a fila inteira).
- **Proposta:** um primitivo só, `WaitFor(posição)`, e marcos anotados na frente ao enviar: último escritor de
  compute ou cópia, último release, última espera de host. Cada local de espera diz **o que** precisa ("o
  escritor desta página", "o release deste semáforo") e espera só até a posição desse marco. `WaitFor` é
  idempotente e custa uma leitura atômica quando já passou. O Lichii fez o começo disso (`WaitForWriters`,
  `WaitForSync`).
- **Cuidado (medido aqui, 3.14 do plano do split):** cada espera que esvazia o ring deixa a thread de trás
  ociosa até a frente voltar. Esperas parciais diminuem isso, mas não zeram. Trocar `Drain` por `WaitFor`
  é feito local a local, cada um com A/B no FE.
- **O que não muda:** o dono do scheduler e a ordem dos chunks, invariantes da seção 7 do plano do split.

### 3.4 L3 — Recursos: memória global geral e texturas por heap

**Memória global (o ponto que mais gerou correções por jogo):**

- **Hoje (verificado):** um acesso global vira uma busca, em tempo de execução, entre os storage buffers que o
  rastreio achou em constant buffers. Sem rastreio, não há buffer. Os atômicos globais de 32 bits dependem da mesma busca, e os de 64 bits nem
  têm caminho (`emit_spirv_atomic.cpp:473-485` lança exceção).
- **Proposta:** um caminho geral de fallback com `VK_KHR_buffer_device_address`. Uma tabela de páginas na
  GPU traduz endereço virtual da GPU do jogo → endereço de dispositivo do buffer do host, e o shader acessa
  por ponteiro físico. O rastreio atual continua como caminho rápido quando acha o buffer; o fallback só
  entra no que hoje falha.
- **Problemas a resolver (por isso é um estudo, não um passo):**
  1. Residência: toda página que o fallback pode tocar precisa de buffer no host antes do draw. O
     buffer cache cria buffers sob demanda; aqui precisaria criá-los no mapeamento da GPU (ou usar
     buffers esparsos).
  2. Coerência: o emulador não sabe o que o shader escreveu por ponteiro. Opções: marcar como escrita toda
     página residente acessível por shaders que usam o fallback (conservador e caro), ou um bitmap de
     sujeira na GPU, atualizado pelo shader e lido nas fences (custo por escrita).
  3. Android: limites e suporte variam. O fallback é desktop primeiro.
- **Ganho de arquitetura:** a família de bugs "ponteiro que o rastreio não achou" deixa de existir. O que
  sobra por jogo é desempenho, que é opcional.

**Texturas:** heap bindless persistente (seção 4.5 de `docs/gpu-produced-data-plan.md`). Os slots do heap
têm versão (princípio 2): TIC escrito, imagem criada, apagada, reescalada ou escrita pela CPU sobem a versão,
e o slot só é refeito quando a versão muda. Isso troca o trabalho por draw (o maior custo do FE) por
trabalho por evento, e serve a qualquer jogo bindless sem atalho novo. O degrau de entrada é trocar os mapas
`image_view_ids`/`sampler_ids` por arrays indexados (Eglantine #256), que já é "um slot por índice de TIC".

### 3.5 L4 — Políticas

Uma tabela única (`video_core/policy.h`), lida uma vez no início:

| Política | Modos | Padrão |
| --- | --- | --- |
| `list_fetch` | `submitted` / `learned` / `exact` | `learned` (gpu-produced-data-plan 4.1.3) |
| `macro_params` | `submitted` / `exact` | depende da fase A |
| `cpu_buffer_read` | `none` / `learned` / `fence` | `learned` (o que existe hoje) |
| `cpu_texture_read` | `none` / `learned` / `exact` | `none` (hoje); `learned` se um jogo precisar |
| `semaphore_order` | `immediate` / `ordered` | `immediate` |
| `invalidate_flush` | `off` / `exact` | `off` |
| `split` | `0` / `1` / `2` | `2` |

- **Sintaxe uniforme:** `EDEN_POLICY=list_fetch=exact,cpu_texture_read=learned`. As variáveis atuais viram
  aliases durante uma transição, com log de aviso.
- **Accurate** = todas em `exact`. `EDEN_GPU_HIGH_PARTS` deixa de existir: vira `EDEN_POLICY`.
- **"Learned"** é sempre o mesmo desenho: começa barato, marca `observed` na página quando o padrão aparece
  e passa ao modo exato só ali (seção 3.2).
- Cada política tem contador no `EDEN_BACK_THREAD_STATS`: quantas vezes o teste barato bastou, quantas foi
  ao caminho caro, e o tempo gasto.

### 3.6 L5 — Plugins por identidade

- Interface única: `match(program_hash, contexto) → handler`. Um handler troca um dispatch ou draw por outro
  trabalho (reset indireto na CPU, upscale no lugar de uma rede, uma macro indireta feita no Vulkan).
- **Regras:** nunca chamados no caminho que não casa (custo = uma busca de hash por programa novo, não por
  draw); cada um com chave para desligar; log na primeira ação; testes com o programa capturado.
- **Migração:** o reset indireto do Bananza (98b6de5) já casa pelo formato do programa e vira o primeiro
  plugin sem mudar de comportamento. A substituição de DLSS (se entrar, seção 3.4 do outro plano) seria o
  segundo.

### 3.7 L6 — Verificação e observabilidade

- **Corpus de shaders** (seção 3.1): roda na nuvem.
- **Replay de frames (longo prazo):** capturar, num frame, as command lists, a memória que elas tocam e o
  estado das engines, e reexecutar sem o jogo (headless; na nuvem com lavapipe, e o Eden já traz
  `tools/llvmpipe-run.sh`). A comparação é por hash da imagem com uma tolerância. Isso permite que eu valide
  mudanças de núcleo sem você em casa. É caro de fazer: precisa de captura determinística e de reproduzir
  o estado dos caches. Começa com um frame do FE e um do Bananza.
- **Depuração ao vivo:** o servidor opt-in com MCP do Lichii (#111), quando o fluxo pedir.

## 4. Como fica a vida com um jogo novo

1. Rodar, juntar o log e os dumps.
2. Rejeições de shader → L0: implementar, fixture, sweep.
3. Imagem errada, mas sem rejeição → tentar políticas em `exact`, uma por vez, com a mesma janela
   (substitui a bissecção de `EDEN_GPU_HIGH_PARTS`). A política que conserta diz o consumidor; o modo
   `learned` dele cobre o jogo sem custo para os outros.
4. Só se nenhuma política consertar, há um buraco de arquitetura, e ele vira uma política nova ou um
   consumidor novo da tabela, não um mecanismo novo.
5. Algo que só o jogo faz e que não vale emular → plugin.

## 5. Ordem de implementação

Cada passo entra sozinho, com A/B no FE (praça e palácio, draws/s com estatísticas desligadas) e sem mudar o
padrão antes de medir.

| # | Passo | Depende | Risco para o FE |
| --- | --- | --- | --- |
| 1 | Registro de políticas + contadores; `EDEN_GPU_HIGH_PARTS` e `EDEN_NX2_MACRO_REFRESH` viram políticas | — | nenhum (mesmo comportamento) |
| 2 | Tabela de estado de página (só `gpu_version` e flags) + consumidor "busca de command list" (fase A do outro plano); reverter o 6d3dd9a no split | 1 | baixo com `learned`; medir |
| 3 | Corpus de shaders + sweep fora do jogo | — | nenhum |
| 4 | `WaitFor(posição)` com marcos; trocar `Drain` local a local | 2 | médio; cada troca com A/B |
| 5 | Plugins: mover o reset indireto | 1 | nenhum |
| 6 | Arrays de ids de textura (#256) | — | baixo; só com ganho medido |
| 7 | Estudo do fallback de memória global (BDA) | 2 | nenhum enquanto for estudo |
| 8 | Heap bindless com slots versionados | 6 | alto; atrás de chave por semanas |
| 9 | Replay de frames | 3 | nenhum |

Os passos 1, 2 e 3 já dão o núcleo maleável para coerência e a esteira da ISA. Do 4 em diante é desempenho
e generalidade.

## 6. O que não muda

- O split frente/trás e o dono do scheduler.
- As otimizações do FE (memos, bancos, tabelas compactas, escritas seguradas, `EDEN_FLUSH_DRAWS`): ficam como
  estão até que o heap (passo 8) as torne desnecessárias, e aí saem com A/B.
- O formato dos commits e dos docs.

## 7. Riscos

- **Abstração cobrando no caminho quente:** a tabela e as políticas ficam fora do draw. O teste de `Ensure`
  é inline e só roda nos consumidores (busca de lista, macro, semáforo, leitura da CPU), nunca por draw.
- **"Learned" erra a primeira vez:** uma página nova pode ser lida velha uma vez. Para listas, isso é um
  frame; para leituras da CPU do jogo, é o comportamento atual dos buffers. Se um jogo não tolerar, a
  política dele vai para `exact`, e só ele paga.
- **Memória da tabela:** ver seção 3.2.
- **Migração longa:** cada passo deixa o emulador funcionando e mais rápido ou igual; não há "big bang".

## 8. Relação com os outros planos

- `docs/gpu-produced-data-plan.md` é o primeiro consumidor da tabela (passo 2).
- `docs/back-thread-split-plan.md` continua valendo para o desempenho do FE; o passo 4 conversa com os
  achados dele sobre `Drain`.

## 9. Perguntas para o revisor

1. A tabela de estado de página deve ser de dois níveis sob demanda, ou um bitmap simples (como o
   Eglantine) basta e as versões ficam só nas estruturas dos caches?
2. Versões por página são suficientes, ou algum consumidor precisa de granularidade menor (listas pequenas
   no meio de páginas que a GPU reescreve parcialmente)?
3. O fallback de memória global por BDA é viável com o buffer cache atual, ou exige mudar como os buffers são
   criados (residência)? Há alternativa mais barata para os atômicos de append?
4. "Accurate = todas as políticas em `exact`" reproduz o Accurate atual em tudo? Há efeitos do nível High
   fora da lista de `accuracy_parts.h` (precisão de queries, `vk_query_cache.cpp:177`; renderização
   condicional, `:1520`) que precisam entrar como políticas?
5. O replay de frames é factível sem reescrever os caches, por exemplo capturando no nível do GPFIFO mais a
   memória lida, e deixando os caches se reconstruírem no replay?
6. A ordem da seção 5 está certa? Há algum passo que deveria vir antes do 2?
