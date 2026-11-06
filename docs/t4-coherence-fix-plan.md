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


## 9. T4.1: o `UsageTracker` conservador e sem UB (desenho; sem código)

**Estado: desenho, sem código.** Escrito sobre `36b19ff`. Pré-requisito de O1 (revisão 1, item 2) e do `blkcmp` assíncrono
(`list-diag-design.md` 3.8.9, ponto 2). Números de linha de `video_core/buffer_cache/usage_tracker.h`, salvo dito.

### 9.1 O contrato

O rastreador guarda **um bit por bloco de 64 B** (`BUFFER_BYTES_PER_BITSHIFT = 6`, `:21`) em palavras de 64 bits, **uma palavra por
4 KiB** (`BUFFER_PAGE_SHIFT = 12`, `:22`). Para toda faixa `[offset, offset + size)` com `size > 0`, o **bloco tocado** é todo
bloco de 64 B que contém ao menos um byte da faixa: do bloco `offset >> 6` ao bloco `(offset + size - 1) >> 6`.

- **T-1 (marcar):** `Track(offset, size)` marca **todos** os blocos tocados.
- **T-2 (reconhecer):** `IsUsed(offset, size)` devolve `true` se **algum** bloco tocado por ela estiver marcado.
- **T-3:** vale para faixa que cruza um limite de 64 B, que cruza a página de 4 KiB do rastreador, que termina exatamente numa
  borda (de 64 B ou de 4 KiB) e para `size` qualquer, **sem deslocamento indefinido** (nenhum deslocamento por 64 ou mais, nenhuma
  largura negativa) e sem estouro de `offset + size`.
- **T-4 (tamanho zero):** nenhum byte, nenhum bloco tocado: `Track` não faz nada e `IsUsed` devolve `false`.
- **T-5 (fora do rastreador):** blocos além da extensão (`pages.size()`) não se representam. `Track` não os marca; `IsUsed`
  devolve `true` se a faixa tocar algum (desconhecido conta como em uso). Uma faixa assim não existe para um buffer consistente
  (o rastreador tem `(tamanho >> 12) + 1` palavras, `:25-28`); é a rede de segurança.
- **Conservador:** marcar ou reconhecer **mais** blocos do que os tocados é permitido (custo de desempenho). Marcar menos, não.
  Os **bytes** que o upload envia continuam exatos; o rastreador só decide a reordenação.

### 9.2 Onde o contrato falha hoje, ou cai em UB (verificado)

Notação: `pg = offset >> 12`, `pe = (offset + size) >> 12` (`:49-50`, `:69-70`), `fb = (offset % 4096) >> 6` (`:89-90`), `nb = min(size, 4096) >> 6`
(`:91`, `:99`), máscara `~0 >> (64 - nb)` (`:92`, `:100`).

| # | Caso | O que acontece (arquivo:linha) | Efeito |
| --- | --- | --- | --- |
| 1 | `size < 64` (por exemplo um uniform de 16 B, ou `size = 0`) | `nb = 0` → `~0 >> 64` (`:92`, `:100`): **deslocamento indefinido** (o UBSan acusa) | UB. Em x86-64, onde o hardware mascara a contagem, a máscara sai `~0`: a **palavra inteira** (4 KiB) é marcada/testada. Conservador por acaso, não por contrato |
| 2 | `size` não múltiplo de 64, ou `offset` não alinhado | `nb` é o **piso** de `size / 64` e ignora o deslocamento dentro do bloco (`:91`). `[100, 164)` toca os blocos 1 e 2, `fb = 1`, `nb = 1`: só o bloco 1 | **Falha T-1 e T-2**: o bloco 2 não é marcado por `Track`, nem testado por `IsUsed`. Uma faixa que cruza o limite entre dois blocos perde um deles |
| 3 | Última página de uma faixa que cruza páginas | `Track` e `IsUsed` chamam `TrackPage/IsPageUsed(pe, alinhado, resto)` com `resto = offset_end - AlignDown(offset_end, 4096)` (`:63-65`, `:82-84`); `nb = resto >> 6` é o piso | **Falha T-1/T-2** para o bloco parcial final (`resto = 100`: marca só o bloco 0, o bloco 1 fica de fora). `resto < 64` cai no caso 1 |
| 4 | Faixa que termina **exatamente** num limite de 4 KiB | `pe = (offset + size) >> 12` aponta a página **seguinte à última tocada**. `resto = 0` → caso 1 (`~0 >> 64`) | A página `pe` (que a faixa não toca) é marcada inteira em x86 (UB); `dirty_end = pe + 1` (`:55`) e o `IsUsed` testa a página inteira seguinte (falso positivo). Se `pe == pages.size()` a faixa é **descartada em silêncio** (`:51-52`, `:71-72`) |
| 5 | `IsUsed` de **uma página só** | depois de `IsPageUsed(pg, ...)` (`:74`), o laço do meio não roda e a **mesma função final** é chamada (`:83-84`) com `pe == pg`: testa `[0, nb(offset_in_page + size))` a partir do **começo da página** | **Falso positivo** (não falha T-2): blocos antes do início da faixa contam; com `offset_in_page + size < 64` é UB. É o "testa a última página desde o começo dela" |
| 6 | `page_end >= pages.size()` | `Track` guarda a faixa em `recent` (`:48`) e **sai sem marcar** (`:51-52`); `IsUsed` devolve `false` (`:71-72`) | **Falha T-1/T-2/T-5** para faixa que passa da extensão. `pe < pg` (estouro de `offset + size`) cai no mesmo desvio |
| 7 | O cache `recent` (`:43-48`) | devolve cedo se o mesmo par `(offset, size)` já foi rastreado desde o último `Reset` (`:30-31`) | **Correto**, dado que o par foi marcado de fato; deixa de ser quando o caso 6 o põe em `recent` sem marcar. Com a conta nova (marca sempre, ou é T-4) volta a ser |
| 8 | `dirty_begin/dirty_end` (`:54-55`, `:30-37`) | só as páginas `[dirty_begin, dirty_end)` são zeradas no `Reset` | Correto se toda palavra marcada está nesse intervalo. Hoje `dirty_end = pe + 1` cobre a marcação do caso 4; com a conta nova tem de cobrir `ultima_pagina + 1` |

A primeira página de uma faixa que cruza (`TrackPage(pg, offset, size)`, `:56`) passa o `size` inteiro: com `nb = min(size, 4096) >> 6`
e a máscara deslocada por `fb`, os bits que passam de 63 se perdem no deslocamento (`:93`, `:101`), o que **é** o desejado (do
`fb` até o fim da palavra). Só o piso (caso 2) é erro ali.

### 9.3 A conta nova

Para `size > 0` e `offset <= UINT64_MAX - (size - 1)` (senão, `last = UINT64_MAX`, que cai em T-5):

```
first_block = offset >> 6
last_block  = (offset + (size - 1)) >> 6
first_page  = first_block >> 6          // 64 blocos por palavra
last_page   = last_block  >> 6
para page em first_page .. last_page:
    lo = (page == first_page) ? first_block & 63 : 0
    hi = (page == last_page)  ? last_block  & 63 : 63
    mask(page) = (~u64{0} << lo) & (~u64{0} >> (63 - hi))      // lo, hi em 0..63: deslocamentos em 0..63
Track:  pages[page] |= mask(page)      (páginas >= pages.size() ignoradas; dirty_begin/end = [first_page, min(last_page, pages.size() - 1) + 1))
IsUsed: algum (pages[page] & mask(page)) != 0, ou alguma página >= pages.size() tocada
size == 0: Track não faz nada; IsUsed = false
```

**Cumpre o contrato.** T-1/T-2: `mask(page)` tem exatamente os bits `lo..hi` da palavra, ou seja, os blocos de `first_block..last_block`
que caem naquela página; as páginas do meio são `~0`. Logo `Track(r1)` marca todo bloco tocado por `r1`, e `IsUsed(r2)` testa todo
bloco tocado por `r2`: se um bloco é tocado por ambas, o bit é o mesmo, e o `&` não é zero. T-3: todo deslocamento está em
`0..63` (`lo` e `63 - hi` são ≤ 63) e `offset + (size - 1)` não estoura pela guarda; nenhuma largura de máscara é calculada por `size`.
T-4: o desvio de tamanho zero. T-5: o desvio para páginas fora da extensão. O piso do caso 2 e os UB dos casos 1, 3, 4, 5 somem
porque `nb` não existe mais. O caso 4 deixa de marcar a página seguinte; o caso 5, de testar blocos fora da faixa.

**Para faixas alinhadas a 64 B** (`offset % 64 == 0`, `size % 64 == 0`, `size > 0`) a conta nova marca **exatamente** os blocos da
faixa; a atual marca os mesmos mais, no caso 4, a palavra seguinte (por UB) e testa no caso 5 mais blocos. Então para elas o
resultado novo **não perde nenhum reconhecimento que o atual dava em blocos tocados**; pode reconhecer **menos** onde o atual
dava falso positivo (casos 4 e 5). Isso é só ganho de reordenação.

### 9.4 Quem chama e o que muda

| Chamador | O que faz | Efeito da mudança |
| --- | --- | --- |
| `Buffer::MarkUsage` (`vk_buffer_cache.cpp:125-128`) → `Track`, e `last_usage_tick` | 18 chamadas em `buffer_cache.h` (cópias entre buffers, bindings de vértice/índice/uniforme/storage/textura, `JoinOverlap`, criação de buffer, downloads) | marca **mais** blocos quando a faixa não é alinhada (casos 2 e 3: bloco parcial) e **menos** quando é pequena ou termina na borda (casos 1 e 4: hoje a palavra inteira, por UB) |
| `Buffer::IsRegionUsed` (`vk_buffer_cache.h:58`) → `IsUsed` | só `BufferCacheRuntime::CanReorderUpload` (`vk_buffer_cache.cpp:448-458`) | idem, no teste |
| `CanReorderUpload` | `MappedUploadMemory` (`buffer_cache.h:2093`) e `InlineMemoryImplementation` (`:2140`) decidem entre o `upload_cmdbuf` (executa antes do cmdbuf principal, sem quebrar a passada de renderização) e a gravação em ordem | uploads antes reordenados podem passar a ser gravados em ordem, e o contrário |
| `Buffer::ResetUsageTracking` (`vk_buffer_cache.h:64`) | `BufferCacheRuntime::TickFrame` (`vk_buffer_cache.cpp:424-428`) | nenhum, exceto `dirty_end` correto |
| `blkcmp` (3.8) | `MarkUsage` na versão assíncrona | passa a poder confiar na marca |

**É custo de desempenho, não de correção.** Marcar mais blocos pode **tirar a reordenação** de uploads que hoje são reordenados;
marcar menos (os casos 1 e 4) pode **devolvê-la**. A reordenação existe para gravar o upload no `upload_cmdbuf`, que executa antes
dos draws do frame, **sem** `RequestOutsideRenderPassOperationContext` nem barreira dentro da passada (`CopyBuffer` com
`can_reorder_upload`, `vk_buffer_cache.cpp:480-486`, contra `:487-499`). **Como medir:** hoje **não há contador** da reordenação
(`grep reorder` em `video_core` só acha `CanReorderUpload` e a opção). Antes de mudar o rastreador, com as estatísticas ligadas,
contar nos dois pontos de `CanReorderUpload`: uploads com `can_reorder` verdadeiro e falso, e os bytes de cada um (`BackStats`,
atrás de `Enabled()`, no mesmo estilo do D1). A/B do usuário no FE, no mesmo lugar: a fração reordenada e os draws/s antes e depois;
o orçamento é o de `architecture-plan.md` 12.1.

### 9.5 Testes propostos (harness, `tools/list-diag-harness/`)

O `UsageTracker` é um header **sem Vulkan** (inclui só `<algorithm>`, `<array>`, `<limits>`, `<vector>`, `common/alignment.h` e
`common/common_types.h`; conferido com `g++ -fsyntax-only -I src`), então roda no harness, nos três builds, com o UBSan.
1. **Os casos do contrato** contra um oráculo (um `std::set` de blocos): faixa dentro de um bloco; que cruza um limite de 64 B
   (`[100, 164)`); que cruza a página (`[4000, 4200)`); que termina exatamente no limite de 64 B e de 4 KiB; que começa no
   limite; ocupando uma página inteira e várias; `size = 1`, `size = 63`, `size = 64`; no último bloco do rastreador; **tamanho zero**
   (`Track` não marca, `IsUsed` falso); além da extensão (`IsUsed` verdadeiro, `Track` sem efeito); `offset + size` estourando
   64 bits. Em cada uma: `Track(r1)` e `IsUsed(r2)` batem com o oráculo (marcação exata; reconhecimento exato).
2. **Aleatório** contra o oráculo: milhares de pares `(offset, size)` com `Reset` no meio; `IsUsed == oracle_overlap`; e a
   propriedade conservadora (nunca `false` quando o oráculo diz `true`).
3. **Equivalência com o atual nas faixas alinhadas a 64 B**: uma cópia congelada do `UsageTracker` de `36b19ff` no teste
   (marcada com `no_sanitize("shift")`, porque o caso 4 dispara o UB) e o novo, sobre as mesmas faixas alinhadas (`size >= 64`):
   `novo_IsUsed ⇒ atual_IsUsed` **não** é exigido (o novo pode ser mais preciso), mas `oracle ⇒ novo` e `oracle ⇒ atual` valem, e
   a diferença entre `atual` e `novo` só ocorre nos casos 4 e 5 (o teste classifica a diferença).
4. **Cache `recent` e `Reset`**: o par repetido não muda o resultado; depois do `Reset` nada fica marcado (inclusive a última
   página e `dirty_end`); faixa que não passa de `dirty_end` anterior.
5. **UBSan limpo** nos três builds, e o teste de cada caso 1 a 6 da tabela 9.2 falha com o rastreador atual (os que não são UB) e
   passa com o novo.
6. **Integração por leitura, no build do usuário:** o contador de reordenação antes e depois (9.4), e a conferência de
   `IsRegionUsed` do `blkcmp` assíncrono (`blkcmp_discard_reorder_unprotected` perto de zero).
