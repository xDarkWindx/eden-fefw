# T4: corrigir a aplicação de escritas seguradas sobre bytes da GPU (rascunho do planejador)

**Estado: rascunho revisado (seção 8, revisão 1), sem código. O1 é candidata a mitigação, não correção completa de G.** Escrito em 2026-11-06 sobre `975a068`. Graus de certeza como nos
outros docs: **verificado** (lido no código, com `arquivo:linha`), **medido** (de quem), **hipótese**. Caminhos
relativos a `src/`.

## 1. O problema (problema B)

**Mecanismo (verificado).**
1. Uma escrita da CPU numa região que a GPU modificou é **segurada**: `OnCPUWrite` devolve `true` quando
   `IsRegionGpuModified` (`video_core/buffer_cache/buffer_cache.h:178-182`), e o escritor só faz o `Collect`
   (`core/memory.cpp`, `HandleRasterizerWrite`).
2. O `GPUDirtyMemoryManager` guarda cada escrita como `TransformAddress{u32 address, u32 mask}`, num atômico de 8 B: uma
   transformação de 2 KiB com um bit por bloco de 64 B (`core/gpu_dirty_memory_manager.h:213-224`). A posição exata da
   escrita dentro do bloco se perde.
3. Na aplicação, `CachedWriteMemory` lê do guest a faixa inteira dos blocos (`buffer_cache.h:159-162`).
   `InlineMemoryImplementation` subtrai a faixa de `gpu_modified_ranges` e sobe esses bytes para o buffer do host
   (`:2116-2136`).
4. Em Switch 2 (`!AsyncBufferDownloads()`) só se baixa o que a CPU leu. Os bytes que só a GPU escreveu, vizinhos da
   escrita dentro do mesmo bloco, não estão no guest, e a aplicação os sobrescreve com o valor velho do guest.

**Evidência (medido pelo usuário; `docs/list-diag-design.md` 8.6, com a interpretação da revisão 7).** O `blkcmp`
compara os bytes antigos da GPU com o payload que os substituiu, nas faixas afirmadas:
- FE: 6 de 193 e 4 de 188 faixas com diferença.
- Bananza: 4 de 65 faixas com diferença.
- Formatos: no FE, floats próximos (0,42512 contra 0,41862); nos dois jogos, zeros onde a GPU tinha dado.

Isso vale para a execução instrumentada; a confirmação sem `Finish` extra está na seção 5.

## 2. O que o T4 precisa garantir (contrato de memória, `architecture-plan.md` 2.2)

**C1.** A aplicação de uma escrita segurada só muda no host os bytes que a CPU escreveu.

**C2.** Os bytes que a CPU escreveu chegam ao host antes de qualquer consumidor posterior da GPU, como hoje. **Escopo
(revisão 1):** a garantia de G supõe payload válido, escrita concluída (M) e nenhuma sobrescrita por um download antigo
(A). Enquanto A e M estiverem abertos, o T4 não garante C2 de forma geral.

**C3.** Escrita que a CPU nunca fez não aparece no host, nem com valor velho nem com zero.

**O T4 não trata, e não pode dizer que trata, três problemas vizinhos (revisão da rodada 4):**

| | Problema | Onde está | Por que o T4 não o resolve |
| --- | --- | --- | --- |
| G | Granularidade de 64 B cobrindo bytes vizinhos | este doc | é o alvo do T4 |
| A | Download antigo publicado sobre uma escrita da CPU ainda não aplicada aos caches | `gpu-contract-inventory.md` 7.1, `list-diag-design.md` 4 | ordem entre pop e aplicação; uma máscara exata não muda essa ordem |
| M | Aviso publicado antes de o `memcpy` terminar | `list-diag-design.md` 3.5 | o `Collect` vem antes do `memcpy` do chamador; uma aplicação no meio sobe bytes velhos |

Guardar posições exatas resolve **G**. Sozinho, não resolve publicação (M), conclusão da escrita (M) nem ordem com
downloads (A).

## 3. Opções para G

| # | Opção | Como | Custo e risco (hipótese, a medir) |
| --- | --- | --- | --- |
| O1 | **Grão menor no gerenciador** | mantém `{u32 address, u32 mask}` e o CAS de 8 B, mas troca `align_bits` de 6 para 2 ou 3. A transformação cobre 32 grãos: 128 B com grão de 4 B, 256 B com grão de 8 B. O recorte do D1 já existe (`CollectT`) | Uma mudança pequena no lugar que já foi corrigido e testado. Escritas grandes viram mais pedaços (`WriteBlock` de 4 KiB: 32 pedaços com grão de 4 B, contra 2 hoje); `back_buffer`, `Gather` e a junção por página recebem mais faixas. **Residual:** escritas menores que o grão (u8, u16 com grão de 4 B) ainda cobrem vizinhos. Com 128 B, o `address >> 7` usa todos os 32 bits, e o endereço mais alto coincide com o marcador `default_transform.address = ~0U`. Isso é inofensivo: o marcador tem máscara 0 e uma escrita ali se junta a ele (verificado na leitura de `CollectOne`, `:100-121`), mas precisa de um teste de borda |
| O2 | **Máscara de bytes só nos blocos parciais** | mantém o grão de 64 B; quando a escrita não cobre o bloco inteiro, registra a máscara de bytes desse bloco numa estrutura à parte, por núcleo e por lote | Exato até o byte. Mas é uma segunda estrutura no caminho do escritor (~1 milhão de avisos/s no FE, medido), com o mesmo problema de capacidade do ledger do diagnóstico (estouro de 44%, medido): no estouro, ela teria de cair num comportamento seguro. Muda o protocolo do `Collect` e do `Gather` |
| O3 | **Juntar os bytes da GPU antes de subir** | na aplicação, para um bloco que cruza `gpu_modified_ranges`, ler os bytes da GPU, sobrepor só os bytes que a CPU escreveu e subir o resultado | Precisa dos mesmos bytes exatos (O1 ou O2) para saber o que sobrepor; e ainda lê da GPU na aplicação (síncrono: picos de 20-30 ms por leitura, medido no `blkcmp`). Só faz sentido se O1 e O2 não puderem dar posições exatas |
| O4 | **Manter o guest coerente** | baixar mais cedo o que a GPU escreve (como `AsyncBufferDownloads` em Switch 1), para que o guest tenha o valor atual e subir o bloco inteiro não estrague nada | Não muda o gerenciador. Mas traz de volta o custo de banda e de espera que o caminho de Switch 2 evita, e mexe em A (mais downloads publicados). Custo provavelmente acima do orçamento (**hipótese**) |

**Proposta do planejador, revisada (seção 8): O1 como mitigação**, que elimina G só para escritas com início e fim
alinhados ao grão. Uma escrita de 4 B desalinhada (`[1, 5)`, com grão de 4 B) ainda leva os vizinhos (`[0, 8)`).
Escolher entre O1 e uma solução exata (O2) é decisão de compatibilidade e desempenho do usuário, com a medição da
seção 4; não é garantia de correção. O1 depende de dois consertos: o laço do `Gather` (`:195`) e o `UsageTracker`
(T4.1). Motivos para O1 como candidata:
- muda pouco, no lugar que o D1 já corrigiu e testou;
- não acrescenta estrutura nova com capacidade a esgotar;
- o residual de O1 é mensurável antes de decidir.

**Achado ao ler o gerenciador (verificado, preexistente, sem efeito nos dados):** `IsValid(PAddr)` testa
`address < 2^39` (`:227-229`), mas é chamado com o campo `u32` da transformação (`:106` e `:168`), e todo `u32` passa.
O marcador vazio (`~0U`, máscara 0) conta como válido. Então, depois de cada `Gather`, o primeiro `Collect` de cada
núcleo com outro endereço toma o `guard` e empilha uma transformação vazia em `back_buffer`, e o `Gather` empilha a sua
no `front_buffer`. As duas têm máscara 0 e não geram faixa; custam uma trava e uma inserção a mais por núcleo e por
`Gather` (~1 800 `Gather`/s no FE, medido no D1). **Correção (T4.0, revisão 1):** a transformação vazia é a de
**máscara zero**, por um predicado que recebe a transformação (`IsEmpty`/`HasWrites`). Não por `address != ~0U`: com
transformação de 128 B, o endereço `0xffffffff` é legítimo. Fica num commit à parte, antes de O1, medido contra o
commit pai.

## 4. Medições antes de escolher o grão (baratas, só com as estatísticas)

Um contador no `BackStats`, no mesmo ponto da contagem do D1 (`core/memory.cpp`, antes de `CollectNotice`). Para os
pedaços segurados, conta tamanho e alinhamento:
- quantos estão alinhados e são múltiplos de 4 B e de 8 B;
- quantos são menores que 4 B;
- quantos **pedaços** do `CollectT` cada grão produziria: com transformação `T = 32 × grão`,
  `pedaços = 1 + ((address % T) + size - 1) / T` para tamanho positivo (revisão 1);
- quantos **grãos** cada escrita toca, que é outro custo (as faixas do `Gather` e da junção).

Isso dá o residual de O1 com grão de 4 B e de 8 B e o aumento de pedaços. Também dá para ver se os grandes vêm de
`WriteBlock`, que o contador por fonte do D1 já separa.

## 5. Confirmação e reprodução (em paralelo; não é condição para o desenho)

- **Reprodução dirigida no harness (antes de qualquer código do T4).** O teste usa o `GPUDirtyMemoryManager` real, um
  `RangeSet` como `gpu_modified_ranges` e dois vetores de bytes, o host e o guest. Roteiro:
  1. a GPU escreve os bytes [0, 32) de um bloco;
  2. a CPU escreve [40, 44);
  3. a aplicação lê do guest e sobe o que o `Gather` devolveu.
  Hoje, os bytes [0, 32) do host voltam ao valor velho do guest: o teste reproduz o B. Com cada opção, o teste diz o que
  sobra (com O1 e grão de 4 B, só [40, 44) sobe). O teste não usa `BufferCache<P>`; o que ele não cobre é dito.
- **`blkcmp` assíncrono (T3a.2b).** Confirma ocorrências nos jogos sem `Finish` extra nem submissões a mais. Ainda é uma
  execução instrumentada (cópias, barreiras, restrição de reordenação), como diz a revisão da rodada 4. As exigências do
  revisor viram o escopo do desenho (`list-diag-design.md` 3.8.2, requisito da versão assíncrona).

## 6. Critérios de aceite do T4 (a detalhar com o desenho escolhido)

- **Aceite principal: a reprodução dirigida.** Dentro do escopo garantido (escritas alinhadas ao grão, no caso de O1),
  o host mantém os bytes da GPU vizinhos à escrita, e as escritas da CPU chegam inteiras.
- **Harness do gerenciador (D1, T3a.1):** equivalência das saídas do `Gather` com o grão novo; diagnóstico com
  `N=1` igual ao de antes, a não ser pela granularidade.
- **Nos jogos:**
  - `blkcmp` (síncrono e assíncrono) com **menos** diferenças nas faixas afirmadas. Ausência de diferenças **não** é
    requisito: A, M, escritas concorrentes e os limites da observação continuam existindo (revisão 1);
  - custo dentro do orçamento (`architecture-plan.md` 12.1), medido como em 7.4.1.
- **Declarado:** A e M continuam em aberto.

## 7. Perguntas para o revisor

1. O1 como primeira candidata faz sentido, ou há algo no `Gather`, na junção por página ou nos consumidores das faixas
   (`OnCachedWrites`) que quebre com grão menor que 64 B?
2. O `IsValid` que sempre aceita (seção 3, achado) deve ser corrigido junto com O1, ou num item à parte antes dele, já que
   muda quantas vezes o `guard` é tomado?
3. A reprodução dirigida descrita na seção 5 basta como "reprodução" para validar opções, ou deve incluir também A e M,
   para mostrar que O1 não os resolve?

## 8. Revisões

### Revisão 1 (sobre `5b88e88`), conferida pelo planejador

| # | O que a crítica disse | Procede no código? | O que muda |
| --- | --- | --- | --- |
| 1 | **Bloqueador:** só mudar as constantes quebra o `Gather`. `mask = continuous_bits < align_size ? (mask >> continuous_bits) : 0` compara uma contagem de bits com o tamanho do grão em bytes. Com grão de 4 B, a máscara com os bits 0-3 e 8 perde o bit 8 | Sim (`gpu_dirty_memory_manager.h:195`). Hoje está certo **por coincidência**: a máscara tem 32 bits e `align_size` é 64, então a condição é sempre verdadeira e o deslocamento é feito num `u64` (`:186`). A condição certa depende da largura da máscara | O conserto do laço do `Gather` passa a ser dependência explícita de O1, com teste de duas sequências de bits na mesma transformação |
| 2 | **Bloqueador de integração:** com O1, faixas pequenas viram comuns e chegam a `CanReorderUpload` → `UsageTracker::IsUsed`, onde `num_bits = min(size, 4096) >> 6` dá 0 abaixo de 64 B (deslocamento de 64: UB) e `size >> 6` não cobre faixa desalinhada que cruza dois blocos | Sim: a junção por página (`gpu.cpp:168-181`) não exige múltiplos de 64 B; o UB foi visto com UBSan (`usage_tracker.h:92`). Na leitura anterior concluí que o `IsUsed` é conservador numa página só no `-O2` e por causa do UB; isso não vale como garantia | Corrigir o rastreador para marcar, de forma conservadora, todos os blocos de 64 B que a faixa toca, sem UB. É pré-requisito de O1. O rastreio de dependências pode ser conservador; os bytes enviados continuam exatos |
| 3 | O1 não cumpre C1 e C3 para toda escrita: uma escrita de 4 B desalinhada (`[1, 5)`) toca dois grãos e leva vizinhos (`[0, 8)`). O2 "só se o residual for relevante" é decisão de compatibilidade e desempenho, não garantia. C2 depende de A e M | Sim | O1 elimina G só para escritas com início e fim alinhados ao grão; nas outras, é mitigação. C2 passa a dizer que a garantia de G supõe payload válido, escrita concluída (M) e nenhuma sobrescrita por A. A escolha entre mitigação de 4 B e solução exata é do usuário, com a medição da seção 4 |
| 4 | `IsValid`: corrigir antes de O1, mas **pela máscara** (`mask == 0` é vazio), não por `address != ~0U`. Com transformação de 128 B, a última abaixo de 2^39 tem endereço `0xffffffff` | Sim, e corrige a minha proposta: teste pelo endereço descartaria essa transformação, com máscara não nula, no `Gather` (`:168`) | **T4.0**, commit à parte, medido sozinho, com quatro testes: coleta com o gerenciador vazio e depois de `Gather`; `Gather` vazio; última transformação representável com máscara não nula; coleta e retirada intercaladas preservando as faixas |
| 5 | A reprodução dirigida precisa de dois complementos: uma escrita pequena ou desalinhada que continua falhando com grão de 4 ou 8 B (o residual); duas sequências de bits na mesma transformação (a perda do `Gather`). Os negativos de A e M devem continuar mostrando os problemas com o grão novo | Sim | Entram na reprodução |
| 6 | Medição: o número de pedaços depende da transformação `T = 32 × grão`: `pedaços = 1 + ((address % T) + size - 1) / T`. Grãos tocados e pedaços são custos diferentes. "`blkcmp` sem diferenças" não pode ser aceite absoluto | Sim | A seção 4 conta pedaços pela fórmula e grãos tocados à parte. O aceite principal passa a ser a reprodução dirigida, preservando os vizinhos dentro do escopo garantido, mais desempenho e validação nos jogos |

**Sequência revisada:**
1. **T4.0:** `IsValid` pela máscara (item 4), medido sozinho.
2. **T4.1:** `UsageTracker` conservador e sem UB (item 2). Já é bug preexistente hoje, com uploads menores que 64 B.
3. **Medição** de tamanhos e alinhamentos das escritas seguradas (seção 4, com a fórmula do item 6).
4. **Decisão do usuário:** O1 como mitigação (grão de 4 ou 8 B, com o conserto do `Gather` e os testes) ou uma solução exata (O2).

A reprodução dirigida e o `blkcmp` assíncrono seguem em paralelo, sem ampliar o diagnóstico.

### Revisão 2 (sobre `be79604`), conferida pelo planejador

- **T4.0 aprovado.** O ABA "vazio → `Gather` repõe o vazio → CAS do escritor" é coberto: o CAS bem-sucedido é a
  publicação efetiva, sem escrita perdida, e o `s2` vê o `lot_started` do `Gather` (`protocol.h:377-386`). Não precisa
  de geração em `current`, mas precisa de um **teste determinístico** dessa intercalação, porque estresse e equivalência
  serial não garantem que ela ocorreu.
- **Os testes residuais não mostrariam o residual de O1.** `AssembleB` (`src/tests/video_core/list_diag.cpp:2780`) põe os
  bytes divergentes da GPU só em [0, 32). Com grão de 4 B, as escritas [41, 45) e [41, 42) subiriam [40, 48) e [40, 44),
  bytes em que host e guest começam iguais, e o C1 passaria escondendo a perda. **Correção:** nos cenários residuais,
  dados divergentes da GPU em [40, 48).
- **O teste das duas sequências** deve montar os endereços a partir do grão candidato, para continuar sendo "duas
  sequências na mesma transformação" quando o grão mudar.
- **W1** é condição de funcionamento do gerenciador, inclusive sem o diagnóstico (inventário 7.6).
- **Próximo passo: o desenho do T4.1.**

