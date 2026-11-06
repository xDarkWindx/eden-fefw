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
| O2 | **Máscara de bytes só nos blocos parciais** | mantém o grão de 64 B; quando a escrita não cobre o bloco inteiro, registra a máscara de bytes desse bloco numa estrutura à parte, por núcleo e por lote | Exato até o byte. Mas é uma segunda estrutura no caminho do escritor (~1 milhão de avisos/s no FE, medido), e uma capacidade a dimensionar. Ela **não herda necessariamente o estouro do ledger do diagnóstico** (revisão 5): o ledger
pode descartar observações; uma estrutura de coerência exata tem de preservar a informação, ou ter um caminho alternativo
correto quando enche. Depende da implementação. Muda o protocolo do `Collect` e do `Gather` |
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

**Implementado (revisão 5): O1 com grão de 4 B, como mitigação EXPERIMENTAL, desligada por padrão.** Em
`core/gpu_dirty_memory_manager.h`:
- **Opt-in e identidade (revisão 6).** A política tem o nome `held_write_grain`, com os modos `legacy-64` (o padrão) e
  `experimental-4`. Hoje a única entrada é o apelido `EDEN_DIRTY_GRAIN`, que separa três casos: **ausente** (padrão,
  `legacy-64`), **válido** (`4` → `experimental-4`; `64` → `legacy-64`, explícito) e **inválido** (qualquer outro valor, o vazio
  inclusive): o padrão é mantido e a partida registra uma linha de **erro** com o valor recebido e dizendo que o padrão foi
  mantido (regra 1 da 7.2 do handoff). Se a configuração inválida deve impedir a partida é decisão da L4; aqui o erro fica
  visível. **Não há parser de `EDEN_POLICY`**: ele nasce com o resolvedor único da L4. Lida uma vez (`SessionHeldWriteGrain`) e
  fixada na sessão: todos os gerenciadores sem grão explícito usam o mesmo. O `System::Impl` registra uma linha na partida com o
  valor efetivo e a origem (`default` ou `EDEN_DIRTY_GRAIN=...`), e o relatório das estatísticas repete o nome da política.
- **Mecanismo, e por quê.** O grão é um `const bool` do gerenciador (junto de `current`); `CollectT` e a iteração do `GatherT`
  escolhem entre **duas instanciações** dos corpos por ele, cada uma com os deslocamentos e máscaras como constantes de compilação.
  Alternativa descartada: deslocamentos em membros `const` e deslocamento variável; medi essa primeiro e o caminho de 64 B ficou
  de 13 a 34% mais lento que o commit pai (mediana de 3 execuções: CAS 9 contra 10-11 ns; guarda 18 contra 22-25 ns, por esse
  deslocamento e por um contador com `lock xadd`, que passou a `load`+`store`). Com as duas instanciações, microbenchmark de uma
  thread (`Collect`, `Gather` a cada 64; mediana de 11 execuções, ns por chamada, contra o commit pai): escrita já coberta no
  mesmo 2 KiB (o caminho mais barato) **9,3 → 10,5 (+1,2 ns, +13%)**; troca de transformação 19,3 → 18,7; aleatório em 64 KiB
  19,3 → 18,6; blocos de 64 B 19,2 → 18,6. **Ou seja, o caminho de 64 B não ficou mais lento, salvo ~1 ns na escrita já coberta**,
  a carga e o desvio do grão (fixo na sessão); o grão de 4 B custa ~20 ns nesse caso (uma troca de transformação por escrita).
  O microbenchmark é de uma thread e sintético: o custo no emulador se confere na rodada do usuário.
- **Conserto do laço do `Gather`** (revisão 1, item 1): `mask = continuous_bits < mask_width ? mask >> continuous_bits : 0`, com
  `mask_width = 32` (a largura da máscara), não o tamanho do grão em bytes. Vale para os dois grãos.
- **O recorte do `CollectT` (D1)** corta na transformação do grão (128 B ou 2 KiB). `HasWrites` (T4.0) não mudou: o endereço
  `0xffffffff` de 128 B é uma transformação legítima, identificada pela máscara.
- **Custo do escritor** (aceite): um contador das vezes que o `CollectOne` toma o `guard` (a troca de transformação). **Corrigido na
  revisão 6:** o contador pertence ao gerenciador (`GuardTakes()`), é **cumulativo** e nunca zera; é incrementado dentro do
  `guard` com `load` e `store` (um escritor por gerenciador, W1; sem operação com `lock`), nenhum no caminho do CAS, e **só com as
  estatísticas ligadas** (um `const bool` do gerenciador, lido uma vez da mesma condição de `EDEN_BACK_THREAD_STATS`, testado dentro
  do `guard`). O relatório (`Dirty grain ...`) lê o valor cumulativo de cada gerenciador e calcula a diferença desde a leitura anterior
  (o valor anterior fica com o leitor, `DirtyGuardReader`). Soma só os gerenciadores do `System`: ele registra o array
  (`DirtyGuardRegistry`, `Register` no construtor do `Impl`, `Unregister` no destrutor, antes de o array morrer) e o relatório lê sob a
  trava do registro, então nenhum gerenciador de teste ou ferramenta entra por acidente e não há alocador de slots. A chave de cada
  valor lido é (registro, índice): um registro novo nunca herda o valor de outro, mesmo no mesmo endereço. Isto mede trocas de
  transformação, não o custo total do escritor.
- **Faixas por lote:** a linha "Invalidation batches" conta faixas em bytes e páginas de 4 KiB, não depende do grão; com 4 B ela
  deve mostrar mais faixas por lote antes da junção, e é o que se confere no emulador.
- **Layout:** o gerenciador cresce 16 bytes (o grão e a flag do contador, 8 com o padding, e o contador, 8). O teste do harness que
  exigia o layout antigo passou a exigir exatamente esse acréscimo.
- **Não compilado na nuvem:** `core/core.cpp`, `core/memory.cpp` e `video_core/back_thread_stats.cpp` (só checagem de sintaxe com
  GCC/clang).

**Continuam abertos:** o **G residual** (escritas desalinhadas ao grão e menores que 4 B ainda levam os vizinhos), **A** (escrita
da CPU sobre dado novo da GPU antes de baixar) e **M** (escrita concorrente/observação perdida). O1 de 4 B não encerra a coerência
de memória, e só vira padrão depois da medição no emulador (desempenho total, custo do escritor, faixas por lote).

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

Isso **dimensiona as escritas fora da garantia de O1** (as não alinhadas ao grão) e o custo estimado em pedaços e grãos. Não
mede quantos bytes vizinhos têm dados da GPU divergentes, nem quantos seriam perdidos de fato (revisão 4). Especificação
(revisão 4):
- denominadores: o total de pedaços segurados e de bytes, por fonte;
- o alinhamento de **endereço e tamanho** a 4 B e a 8 B, em categorias que se sobrepõem;
- a **linha de base de 64 B** (pedaços e blocos de hoje), para comparar o aumento;
- tamanho zero tratado antes das fórmulas;
- só com as estatísticas, no relatório de 4 s, para que a carga e a cena estável se separem pelo horário. Também dá para ver se os grandes vêm de
`WriteBlock`, que o contador por fonte do D1 já separa.

### 4.1 Resultado (rodada 6, 2026-11-06; FE no lugar de sempre, sem diagnóstico, build `80fa234`)

Medido pelo usuário, uma rodada. A carga e a cena se separam pelo horário: a cena começa aos 34 s, o primeiro relatório
acima de 40 mil draws/s.

| | Carga (24-30 s) | Cena estável (42-54 s) |
| --- | --- | --- |
| Pedaços segurados | ~50-60 mil/s, ~6,5 B cada | ~1 milhão/s, ~4,7 B cada |
| Fonte | 100% `Write<T>` | 100% `Write<T>` (sem `WriteBlock`, `ZeroBlock` ou operação de cache) |
| Menores que 4 B | 0,12-0,19% | 0,02% |
| Endereço **e** tamanho alinhados a 4 B | 99,8-99,9% | **~100%** (o relatório arredonda para 100,0%) |
| Endereço e tamanho alinhados a 8 B | 60-66% | **17-18%** (~41% só o endereço alinhado, ~41% nenhum dos dois) |
| Pedaços do `CollectT` por escrita (grão 4, grão 8, base de 64 B) | 1,000 / 1,000 / 1,000 | 1,000 / 1,000 / 1,000 |
| Grãos tocados por escrita (grão 4 / 8 / 64 B) | 1,60-1,66 / 1,00 / 1,00 | 1,17-1,18 / 1,00 / 1,00 |

**Leitura:**
- **Grão de 4 B:** cobre na garantia de O1 praticamente todas as escritas seguradas do FE, ~100% na cena (menos as 0,02%
  menores que 4 B) e ~99,7-99,8% na carga. Ficam de fora as menores que 4 B e as desalinhadas, 0,2-0,3% na carga, que é onde o `blkcmp` viu o B
  (`list-diag-design.md` 8.7).
- **Grão de 8 B:** não serve. Cerca de 40% das escritas da cena (4 B em endereço `4 mod 8`) arrastariam 4 bytes vizinhos.
- **Custo no escritor (corrigido, revisão 5):** nenhuma escrita vira mais de um pedaço do `CollectT`, mas isso **não** quer
  dizer custo zero. Com transformação de 128 B em vez de 2 KiB, escritas vizinhas caem mais vezes em transformações
  diferentes. Duas células adjacentes de 4 B podem ficar em transformações diferentes. Isso aumenta as trocas de
  transformação, as tomadas do `guard`, as inserções em `back_buffer` e as tentativas de CAS.
- **Custo na aplicação:** escritas não adjacentes no mesmo bloco de 64 B deixam de ser uma faixa só, e o `Gather`, a junção
  por página e o `OnCachedWrites` recebem mais faixas (**hipótese**; até 16 por bloco).
- **Os dois custos entram no aceite**, medidos no emulador: o desempenho total e as faixas por lote.
- **Residual:** a frequência pequena das escritas fora da garantia não mostra que o efeito delas seja pequeno.
- **Limites:** é um jogo e uma rodada. Isto dimensiona as escritas fora da garantia de O1; não mede quantos vizinhos
  divergentes elas teriam (revisão 4).

**Bananza (rodada 7, mesmo build, uma rodada; medido pelo usuário).** A emulação do jogo não está completa: carrega, mostra
as cutscenes e entra na caverna. O usuário ficou ~26 s dentro da caverna, andando e apertando botões. A cena começa aos
~53 s.

| | Carga (32-40 s) | Caverna (57-77 s) |
| --- | --- | --- |
| Pedaços segurados | ~2-28 mil/s, ~4 B cada | **~1,6-1,8 milhão/s, ~8 B cada** |
| Fonte | 100% `Write<T>` | 100% `Write<T>` |
| Menores que 4 B | 0,00% | 0,04% |
| Endereço e tamanho alinhados a 4 B | 100,0% | 100,0% |
| Endereço e tamanho alinhados a 8 B | 6,6% | 99,0-99,2% |
| Pedaços do `CollectT` por escrita (grão 4 / 8 / 64 B) | 1,000 / 1,000 / 1,000 | 1,000 / 1,000 / 1,000 |
| Grãos de 4 B tocados por escrita | 1,07 | 1,99 (escritas de 8 B: dois grãos contíguos) |

**Nos dois jogos:**
- 100% das escritas seguradas vêm de `Write<T>`.
- Praticamente todas são alinhadas a 4 B, e as menores que 4 B não passam de 0,2% (FE na carga) e ficam em 0,02-0,04% na
  cena.
- O alinhamento a 8 B varia por jogo e por fase: 17% na cena do FE, 99% na caverna do Bananza, 6,6% na carga do Bananza.

**Conclusão para a escolha:** o grão de 4 B põe ~99,7% a ~100% das escritas seguradas dos dois jogos na garantia de O1, sem
dividir nenhuma em mais pedaços; o custo do escritor (trocas de transformação e `guard`) continua a medir. As escritas de
8 B do Bananza tocam dois grãos contíguos, que continuam numa faixa só **se** caírem na mesma transformação de 128 B. O de 8 B
fica descartado pelo FE. Isto continua dimensionando, não medindo perda, e são dois jogos com uma rodada cada.

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
  **Estado (O1 de 4 B, harness):** o cenário PRINCIPAL (`[40, 44)`, GPU divergente em `[0, 32)`) e o residual de 4 B **alinhado**
  (`[40, 44)` com GPU divergente também em `[40, 48)`, caso novo) cumprem o C1 com o grão de 4 B, e os casos correspondentes
  perderam o `[!shouldfail]`. Caso a caso:
  - `t4 [a]` principal, 64 B: igual (o `Gather` dá `[0,64)`, o host perde `[0,32)`).
  - `t4 [b]` principal: **dividido em dois**. Com 64 B (o padrão) continua `[!shouldfail]`: o defeito do padrão segue
    demonstrado. Com 4 B (`t4 [a4]`, `t4 [b4]`) o `Gather` dá `[40, 44)`, o host guarda `[0, 32)` e recebe `[40, 44)`: C1 cumprido,
    sem a tag.
  - `t4 [a]` residual (`[41,45)` e 1 B em 41), 64 B: igual (a preparação do defeito de hoje).
  - `t4 [b]` residual, escrita desalinhada `[41, 45)` e de 1 B em 41, com GPU divergente em `[40, 48)`: **continuam
    `[!shouldfail]` com o grão de 4 B** (`t4 [b4]`); `t4 [a4]` descreve o que levam (`[40, 48)` e `[40, 44)`). É o residual de O1.
  - novo: `t4 [b4]` residual, 4 B alinhado em `[40, 44)`: C1 cumprido, e `[44, 48)` guarda os bytes da GPU.
  - o teste das duas sequências de bits na mesma transformação roda nos dois grãos (e uma terceira sequência, e o bit 31); com a
    condição do laço no tamanho do grão em bytes (o defeito da revisão 1) ele falha com 4 B.
- **Harness do gerenciador (D1, T3a.1):** equivalência das saídas do `Gather` com o grão novo (feita: mesma cobertura em bytes
  para escritas alinhadas a 64 B; com 4 B a cobertura contém os bytes escritos e fica dentro da de 64 B); bordas de 128 B e de
  2 KiB (faixa exata, cruzando, terminando na borda); o endereço `0xffffffff` de 128 B; o contador do escritor. A bateria do
  protocolo dos lotes, P0, ABA e cobertura roda com os **dois** grãos (`run.sh`): os testes que tratam de blocos de 64 B
  arredondam a escrita ao bloco com o grão de 4 B (a diferença é a transformação de 128 B, que o protocolo tem de aguentar), e os
  que têm um grão como assunto o fixam explicitamente.
- **Nos jogos:**
  - `blkcmp` (síncrono e assíncrono) com **menos** diferenças nas faixas afirmadas. Ausência de diferenças **não** é
    requisito: A, M, escritas concorrentes e os limites da observação continuam existindo (revisão 1);
  - custo dentro do orçamento (`architecture-plan.md` 12.1), medido como em 7.4.1, **com** o custo do escritor (trocas de
    transformação) e as faixas por lote.
- **Declarado:** G residual, A e M continuam em aberto.

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

**Estado: implementado (commit B do T4.1); a chave temporária `EDEN_USAGE_TRACKER_OLD=1` foi removida em `d5e7158` (revisão 4), e o rastreador novo é o único; o texto abaixo descreve o que foi implementado, com os ajustes da revisão 3 (o que fala da chave é histórico).** Escrito sobre `36b19ff`. Pré-requisito de O1 (revisão 1, item 2) e do `blkcmp` assíncrono
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

Implementado em `video_core/buffer_cache/usage_tracker.h` (`Track`, `IsUsed`, `BlocksOf`, `PageMask`). Com os ajustes da revisão 3:

```
Track(offset, size):
    size == 0: retorna, ANTES de qualquer `size - 1`
    cache `recent` (par exato já rastreado desde o Reset): retorna
    (first_block, last_block) = (offset >> 6, saturado(offset + (size - 1)) >> 6)      // saturado: não dá a volta em 64 bits
    (first_page, last_page)   = (first_block >> 6, last_block >> 6)                      // 64 blocos por palavra
    pages vazio, ou first_page >= pages.size(): retorna SEM mexer em dirty_begin/dirty_end   // sem interseção
    last = min(last_page, pages.size() - 1)                                              // só a interseção com as páginas que existem
    dirty_begin = min(dirty_begin, first_page);  dirty_end = max(dirty_end, last + 1)
    para page em first_page .. last:  pages[page] |= PageMask(page)
IsUsed(offset, size):
    size == 0: false
    last_page >= pages.size(): TRUE na hora (faixa não vazia que passa da extensão)
    para page em first_page .. last_page:  (pages[page] & PageMask(page)) != 0  ->  true
PageMask(page): ~0 para as páginas do meio (palavra inteira: nenhum trabalho por bloco);
    nas pontas, lo = first_block & 63 (primeira página) ou 0, hi = last_block & 63 (última) ou 63;
    mask = (~0 << lo) & (~0 >> (63 - hi))      // lo e hi em 0..63: nenhum deslocamento de 64
```

`pages.size() - 1` só é calculado depois de `pages` ter ao menos uma palavra e de `first_page < pages.size()`. O laço de `Track`
percorre no máximo `pages.size()` palavras (a interseção), nunca até o último endereço saturado. O modo antigo (`TrackOld`,
`IsUsedOld`, `TrackPage`, `IsPageUsed`) **saiu do header em `d5e7158`**; a cópia congelada fica em `src/tests/video_core/usage_tracker_legacy.h`.

**A chave temporária (REMOVIDA em `d5e7158`; texto histórico).** `EDEN_USAGE_TRACKER_OLD=1` ligava o rastreador **antigo**; o novo é o padrão. É lida uma vez na partida
(`UsageTrackerOldMode`) e fixada por instância no construtor, valendo para `Track` e `IsUsed` **juntos**, sem troca durante a execução. **Existia só para o
A/B**; o modo antigo conserva o UB conhecido (9.2). A linha "Upload reordering" do relatório deixou de dizer qual rastreador está ativo. Uma rodada por braço é triagem: se os números ficarem próximos, repete-se em ordem alternada.

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

**O que o rastreador novo muda em cada grupo de `MarkUsage` (revisão 3, item 6; números de linha de `buffer_cache.h` de hoje, as
unidades continuam deslocamento e tamanho em bytes dentro do buffer):**

| Grupo (linhas) | O que marca | O que o rastreador novo muda |
| --- | --- | --- |
| Uniforms gráficos e de compute (`:1073`, `:1384`) | `(offset, size)` do uniform; `size` pode ser pequeno ou zero | `size = 0` deixa de marcar a palavra inteira (era UB): não marca nada e `IsUsed` é falso. `size < 64` marca só o(s) bloco(s) tocado(s), não os 4 KiB. Perde só os falsos positivos antigos; nunca um bloco tocado |
| Storage segmentado (`:1239`) | cada segmento marca `(buffer.Offset(segment.device_addr), segment.size)` | as bordas parciais passam a marcar o bloco parcial do começo e do fim (o piso antigo os perdia); segmentos que terminam numa borda de 4 KiB deixam de marcar a palavra seguinte |
| Vértices (`:902`, `:945`) | `(offset, binding.size)`, faixas grandes e repetidas | o cache `recent` continua (par idêntico = retorno imediato) e as palavras do meio são `~0` inteiras, sem trabalho por bloco; só as duas pontas são calculadas. O custo por faixa é o de hoje |
| `JoinOverlap` e criação (`:1905`, `:1927`) | `(dst_offset, size)` da cópia e `(0, size_bytes)` do clear | continuam impedindo que um upload posterior passe à frente; a faixa que termina exatamente na borda marca até o último bloco tocado e não a palavra seguinte. `(0, size_bytes)` cobre a extensão inteira, e o último byte cai na última palavra (`(tamanho >> 12) + 1` palavras) |
| Downloads (`:692`, `:2244`; era `:2232` na revisão) | `(copy.src_offset, copy.size)`, leitura da GPU | "uso" não é só escrita: um download também impede a sobrescrita antecipada do que ele lê; mesma marcação exata, e o bloco parcial final deixa de ficar de fora |
| Cópias entre buffers e demais (`:275`, `:276`, `:311`, `:824`, `:1270`, `:1300`, `:1335`, `:1410`, `:1441`) | bindings e cópias, `(offset, size)` | a mesma regra; nenhuma muda de unidade |

**Eliminar falsos negativos é correção de ordem de execução, não só desempenho** (revisão 3, item 3): um bloco tocado e não
reconhecido deixa um upload passar à frente de um uso que ele não podia ultrapassar. O que muda o **desempenho** é a precisão a
mais: marcar mais blocos (casos 2 e 3) pode **tirar a reordenação** de uploads que hoje são reordenados; marcar menos (os casos 1
e 4, onde hoje a palavra inteira era marcada por UB) pode **devolvê-la**. A reordenação existe para gravar o upload no `upload_cmdbuf`, que executa antes
dos draws do frame, **sem** `RequestOutsideRenderPassOperationContext` nem barreira dentro da passada (`CopyBuffer` com
`can_reorder_upload`, `vk_buffer_cache.cpp:480-486`, contra `:487-499`). **Como medir (implementado):** com as estatísticas ligadas, a linha "Upload reordering" do relatório separa **permissão** (o que
`CanReorderUpload` disse nos dois chamadores, `MappedUploadMemory` e `InlineMemoryImplementation`: pode / não pode, com os bytes) de
**efeito** (o caminho que `BufferCacheRuntime::CopyBuffer` tomou: `upload_cmdbuf`, só quando a origem é o stream buffer **e** há
permissão; em ordem vindo do stream buffer; em ordem, outras cópias; com os bytes). Permissão não é efeito (revisão 3, item 5). A/B do usuário no FE, no mesmo lugar: a fração reordenada e os draws/s antes e depois;
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
3. **O oráculo é a referência normativa** (revisão 3, item 2); nada de `no_sanitize` no legado, que desliga a detecção e não define o
   resultado. O legado (cópia congelada de `36b19ff` em `src/tests/video_core/usage_tracker_legacy.h`) é comparado **só nas
   operações em que está definido** (`size >= 64` e, se a faixa cruza palavras, resto da última palavra `>= 64`), sob o UBSan: nas
   faixas alinhadas o novo é igual ao oráculo e o legado nunca perde um bloco tocado, mas pode dizer "em uso" onde o oráculo diz
   não (caso 5); nas não alinhadas o legado perde blocos (caso 2). Os casos que caem em UB no legado ficam em **um teste à
   parte** (`T4.1 [6]`), que só calcula a contagem do deslocamento (64) e não executa o legado.
4. **Cache `recent` e `Reset`**: o par repetido não muda o resultado; depois do `Reset` nada fica marcado (inclusive a última
   página e `dirty_end`); faixa que não passa de `dirty_end` anterior.
5. **UBSan limpo** nos três builds, e o teste de cada caso 1 a 6 da tabela 9.2 falha com o rastreador atual (os que não são UB) e
   passa com o novo.
6. **Integração por leitura, no build do usuário:** o contador de reordenação antes e depois (9.4), e a conferência de
   `IsRegionUsed` do `blkcmp` assíncrono (`blkcmp_discard_reorder_unprotected` perto de zero).

### Revisão 3 (sobre `29ea216`): testes aprovados, T4.1 com ajustes antes do código

Conferida pelo planejador. **O corpo da seção 9 é atualizado pelo codador junto com a implementação, para não divergir disto.**
1. **O laço é limitado antes de começar (9.3).**
   - O caso vazio é tratado antes de calcular `size - 1`.
   - `Track` percorre só a interseção com as páginas existentes. Sem interseção, retorna sem mexer em
     `dirty_begin`/`dirty_end`.
   - `IsUsed` devolve `true` na hora se uma faixa não vazia passa da extensão.
   - Nunca usar `pages.size() - 1` sem garantir que há páginas.
   - Assim, T-5 fecha também em custo, sem laço gigante com o último endereço saturado em `UINT64_MAX`.
2. **A referência normativa dos testes é o oráculo de blocos (9.5).** Nada de `no_sanitize("shift")` no legado: isso
   desliga a detecção, mas não define o resultado. O legado é comparado só onde as operações são definidas; os casos de
   UB ficam à parte, como demonstração do defeito antigo.
3. **9.4: eliminar falsos negativos é correção de ordem de execução**, não só desempenho. A precisão a mais é que
   muda o desempenho.
4. **Chave temporária aceita** (`EDEN_USAGE_TRACKER_OLD=1`):
   - o rastreador novo é o padrão;
   - a chave é lida uma vez na partida e vale para `Track` e `IsUsed` juntos, sem troca durante a execução;
   - não vira opção permanente, porque o modo antigo conserva um UB conhecido;
   - uma rodada por braço é triagem: se os números ficarem próximos, repete-se em ordem alternada.
5. **O contador de reordenação separa permissão de efeito.** `CanReorderUpload == true` é só permissão; a reordenação
   efetiva também exige `src_buffer == staging_pool.StreamBuf()` (`vk_buffer_cache.cpp:481`, verificado). Contar a
   elegibilidade **e** o caminho escolhido em `CopyBuffer`, com nomes diferentes.
6. **`MarkUsage` com atenção especial** (as unidades continuam deslocamento e tamanho em bytes):

   | Chamadores | O que verificar |
   | --- | --- |
   | Uniforms gráficos e de compute (`buffer_cache.h:1073`, `:1384`) | faixas pequenas, possivelmente de tamanho zero: perdem os falsos positivos antigos, nunca os blocos tocados |
   | Storage segmentado (`:1239`) | cada segmento marca o seu próprio deslocamento; as bordas parciais importam |
   | Vértices (`:902`, `:945`) | faixas grandes e repetidas: manter o cache `recent` e o preenchimento de páginas inteiras, sem trabalho por bloco |
   | `JoinOverlap` e criação (`:1905`, `:1927`) | cópias e clears continuam impedindo que uploads posteriores passem à frente; testar faixa que termina na borda |
   | Downloads (`:692`, `:2232`) | são leituras da GPU que também impedem sobrescrita antecipada: "uso" não é só escrita |

### 9.6 Pendência de desempenho do T4.1 (rodada 5, `list-diag-design.md` 8.7)

O tempo da thread de trás em `stage buffers` subiu de ~7,2% (rastreador antigo, uma rodada) para ~9,3-9,7% (novo, duas
rodadas), no trecho final. Em draws/s, na janela escolhida, não se demonstrou regressão líquida (−0,9%, menos que a variação
entre as duas rodadas do novo). Hipótese de partida: no `Track` novo, as faixas grandes que não batem no cache `recent`
calculam `PageMask` por página, com dois desvios, onde o antigo fazia o meio como uma sequência de `~0`.

**Investigação (sem mudar o rastreador; sem rodada nova no jogo).**

*Que `Track` caem em `stage buffers`.* O trecho é `BindHostStageBuffers(stage)` (`vk_graphics_pipeline.cpp:989`), isto é,
uniforms, storage e texture buffers por estágio (`buffer_cache.h:410`). Os `MarkUsage` dali:

| Chamador | Faixa | Observação |
| --- | --- | --- |
| Uniform, caminho em cache (`:1073`) | tamanho do uniform (pequeno, até o limite de `uniform_buffer_skip_cache_size` ou mais se o dado é da GPU) | só quando o uniform **não** vai pelo stream buffer e precisa de bind |
| Storage, uma faixa (`:1270`) | `binding.size` do storage, de centenas de bytes a MiB | a cada draw que religa |
| Storage, multi-faixa (`:1239`) | cada segmento no seu deslocamento | idem |
| Texture buffer (`:1300`) | tamanho do texel buffer | |
| `SynchronizeBuffer` → `UploadMemory`/`MappedUploadMemory` → `CanReorderUpload` → `IsUsed` (`:2093`) | `dst_offset`, `size` de cada cópia | poucas por segundo (centenas a milhares), não por draw |

Os vértices (`:902`, `:945`) **não** estão em `stage buffers`: caem em `UpdateGraphicsBuffers`/`BindHostGeometryBuffers`, o
trecho `buffers`, e só quando a flag de sujo do vértice está ligada. `JoinOverlap`, criação e downloads também não. A hipótese,
como foi escrita ("faixas grandes e repetidas de vértices"), aponta para o trecho errado; o que vale é qualquer faixa
grande e **fora do `recent`** que um storage ou texture buffer religue.

*Microbenchmark* (`tools/usage-tracker-bench/`, `run.sh`; -O2, sem sanitizadores, só faixas em que o antigo é definido:
tamanho ≥ 64 e, se cruza uma palavra de 4 KiB, termina a ≥ 64 B dentro dela; o antigo é a cópia congelada
`usage_tracker_legacy.h`). Mediana de 9 passadas, ns por chamada, uma máquina compartilhada de 4 núcleos na nuvem (os números
pequenos oscilam de uma execução para outra em ±30%; os grandes não). `Track`, com `Reset` a cada 200 passadas:

| Cenário (tamanhos como nos chamadores acima) | novo | antigo |
| --- | --- | --- |
| uniforms pequenos, 256 distintos (erram o `recent`) | 15-25 | 16-22 |
| uniforms pequenos, 8 repetidos (acertam) | 4-12 | 4-6 |
| storage segmentado 64 B-256 KiB, 200 distintos | **43-68** | 17-23 |
| storage, 12 repetidos de até 1 MiB (acertam) | 8-11 | 8-9 |
| vértices 4-32 MiB, 3 repetidos (acertam; a 1ª de cada `Reset` erra) | 17-34 | 11-14 |
| vértices 1-16 MiB, 24 ciclando (erram sempre) | **1 560-2 490** | 255-280 |
| vértice de 8 MiB com deslocamento mudando (erra sempre) | **1 520-1 920** | 103-108 |
| termina a 64 B de uma borda de 4 KiB | 15-18 | 14-15 |

`IsUsed` (destinos de upload contra um rastreador com ranges já marcados): faixas de até 4 KiB, 4-5 ns contra 3,5; de 4-64 KiB,
11 contra 7; de 64 KiB-1 MiB, 133 contra 85; de 1-8 MiB, 555 contra 290-330. Mesmas respostas (as contagens de `true`
coincidem nos dois), como se espera onde o antigo é definido.

*Leitura.* **Hipótese confirmada no microbenchmark:** quando a faixa é grande e erra o `recent`, o `Track` novo custa ~0,75
ns por palavra de 4 KiB contra ~0,05-0,13 ns do antigo (6 a 18 vezes), porque o antigo preenche o meio com uma sequência de
`~0` (vetorizável) e o novo calcula `PageMask` por palavra. Nas faixas pequenas ou que acertam o `recent` não há diferença
que se distinga da oscilação. O `IsUsed` fica 1,5 a 2 vezes mais lento nas faixas de várias palavras, mas é chamado só por
upload.

*O que isto não mostra.* (1) Que esse custo explica os ~2 pontos percentuais de `stage buffers` no jogo. Ordem de grandeza:
com ~80 mil draws/s e a thread de trás cheia, 2 pontos são ~250-300 ns por draw, ou cerca de 400 palavras (1,6 MiB) de faixa
que erra o `recent` por draw, a ~0,7 ns de diferença por palavra. É plausível para storage buffers grandes que mudam, mas **não
está medido**: não há contador do tamanho em palavras dos `Track` que erram o `recent` por chamador. (2) As faixas do
benchmark são sintéticas. Fica pendente, se o autor quiser fechar a causa antes de corrigir, um contador de estatística (palavras
preenchidas nos `Track` que erram o `recent`, por chamador), e comparar o trecho `buffers`: se a causa estivesse nos vértices, esse
trecho também teria subido, e a 8.7 não o traz.

*Correção implementada (neste commit; não muda o contrato).* `Track` aplica `FirstMask` na primeira palavra e `LastMask` na última da
interseção e preenche as do meio com `std::fill(..., ~u64{0})`, sem desvio por palavra; uma faixa de uma palavra só usa
`FirstMask & LastMask`; a última palavra recortada pela extensão é tratada como inteira. `IsUsed` tem o mesmo formato: máscara na
primeira e na última, `pages[i] != 0` nas do meio, saindo na primeira verdadeira; fora da extensão continua `true`. Mantidos: o caso
vazio antes de `size - 1`, só a interseção com a extensão, `dirty_*` intocados sem interseção, o cache `recent`, e nenhum
deslocamento fora de 0..63. Os testes T4.1 [1]-[6] contra o oráculo passaram sem ajuste; o [8] é novo (faixas de 1, 2, 3 e mais
palavras, primeira = última, faixas recortadas pela extensão, o estado inteiro conferido bloco a bloco). Duas mutações de
sensibilidade (o `fill` curto em uma palavra e o laço do meio do `IsUsed` curto em uma) são pegas pelo [8] e pelos antigos.

Antes e depois, `tools/usage-tracker-bench/run.sh`, ns por chamada (duas execuções de cada; o antigo é a cópia congelada, na mesma
execução, como régua):

| Cenário | novo antes | novo depois | antigo (régua) |
| --- | --- | --- | --- |
| uniforms pequenos, 256 distintos | 15-19 | 15 | 12-23 |
| uniforms pequenos, 8 repetidos (acertam) | 4-6 | 3,5 | 3-5 |
| storage segmentado 64 B-256 KiB | 43-70 | 12 | 13-24 |
| storage, 12 repetidos (acertam) | 6-10 | 6-7 | 6-10 |
| vértices 4-32 MiB, 3 repetidos (acertam) | 21-30 | 8-10 | 9-11 |
| vértices 1-16 MiB, 24 ciclando (erram) | **1 570-2 075** | **259-311** | 249-341 |
| vértice de 8 MiB com deslocamento mudando (erra) | **1 640-1 700** | **107-139** | 103-141 |
| termina a 64 B de uma borda de 4 KiB | 15-16 | 11-18 | 10-16 |
| `IsUsed` até 4 KiB | 4,1 | 2,9-3,5 | 3,1-4,5 |
| `IsUsed` 4-64 KiB | 11 | 5,6-8,7 | 5-9 |
| `IsUsed` 64 KiB-1 MiB | 134-138 | 58-80 | 58-83 |
| `IsUsed` 1-8 MiB | 555-568 | 170-175 | 181-320 |

As linhas de faixa grande com erro no `recent` voltaram ao patamar do antigo (1 570-2 075 → 259-311 ns, e 1 640-1 700 → 107-139),
e as demais não pioraram além da oscilação (máquina compartilhada de 4 núcleos; as linhas pequenas variam ±30% entre execuções).
As contagens de `true` do `IsUsed` coincidem nos dois rastreadores.

**Ressalva:** o efeito no jogo (o trecho `stage buffers` voltar de ~9,5% para ~7%) **não** está medido; confere-se na rodada do
usuário. O microbenchmark só mostra o custo do `Track` e do `IsUsed` isolados, com faixas sintéticas.

**Resultado da correção (rodada 6, medido pelo usuário):** `stage buffers` voltou a ~6,7% do tempo da trás na cena estável
(antes da correção, 9,3-9,7%; rastreador antigo, 7,2%). Os draws/s alinhados pelo início da cena dão 77 569, dentro da
faixa das rodadas 5 (77 308 a 81 918). **A pendência 9.6 está resolvida.** A trás ficou em ~83% de ocupação, contra 86-87%
antes; pode ser o custo do contador novo de tamanhos nas threads do jogo, que só roda com as estatísticas (**hipótese**).

### Revisão 4 (sobre `0e5ad1e`)

- **A chave `EDEN_USAGE_TRACKER_OLD` e o modo antigo saem do código de produção** (feito, chave removida em `d5e7158`); a cópia congelada fica nos testes (os testes que alternavam a chave saíram; os que documentam o defeito antigo seguem contra a cópia). A
  decisão se apoia no contrato e no fim do UB, e não encerra a investigação de desempenho (9.6).
- O contador da seção 4 segue com a especificação acima.
- A escolha entre O1 e O2 preserva a distinção: O1 é mitigação para escritas alinhadas ao grão. Ter poucas escritas fora
  do alinhamento não transforma O1 numa solução exata.

### Revisão 5 (sobre `5dfcbe9`): O1 de 4 B como mitigação experimental

- **Decisão:** O1 com grão de 4 B, **como mitigação experimental**. Virar padrão depende dos testes e do custo medido no
  emulador, tanto o total quanto o do escritor e o da aplicação.
- **Corrigido no corpo (4.1 e 3):**
  - o custo no escritor não é zero (transformação de 128 B: mais trocas, `guard`, `back_buffer` e CAS);
  - "100% alinhadas" vira "~100%", porque as 0,02-0,04% menores que 4 B existem, e a frequência pequena do residual não
    mostra efeito pequeno;
  - O2 não herda necessariamente o estouro do ledger.
- **Para o código:**
  - O1 entra junto com o conserto do laço do `Gather`;
  - testes com o grão real de 4 B: bordas de 128 B, sequências separadas de bits e os negativos do residual;
  - medição do desempenho total e das faixas por lote.
- **Continuam abertos:** o G residual, o A e o M. O O1 não encerra a coerência de memória.
- A revisão de `d5e7158`, `1b0d10d`, `86aec48` e `5aab322` já foi respondida:
  - a remoção da chave e a otimização do rastreador foram aprovadas;
  - o contador de tamanhos está funcionalmente correto, com a ressalva do seu custo e da interferência dele na comparação
    de desempenho, já que roda nas threads do jogo com as estatísticas ligadas.

### Revisão 6 (diff do O1, `9cd24b6`; conferida pelo planejador)

- **O mecanismo dos dois grãos está aprovado por leitura:**
  - o grão é fixado na construção;
  - coleta e entrega usam a mesma especialização, cada uma com constantes próprias;
  - as ordens de memória foram preservadas;
  - o laço do `Gather` usa a largura de 32 bits;
  - os testes cobrem as bordas, o endereço máximo e os casos que o O1 não resolve.
- **Bloqueador do A/B: o contador do `guard`.**
  1. **Reset:** o relatório zera o contador com `exchange(0)` sem o `guard` (`gpu_dirty_memory_manager.h:55`), enquanto o
     escritor faz `load` e `store` sob ele. Um incremento que atravessa o reset faz contagens antigas reaparecerem. O
     TSan não acusa, porque todos os acessos são atômicos.
  2. **Slots sem dono:** o slot sai de `fetch_add(...) % 16` (`:97`), sem reserva. Um gerenciador antigo vivo e um novo
     podem dividir o slot, com `guard`s diferentes, e perder incrementos.
  **Correção:**
  - um contador **cumulativo** que pertence ao gerenciador, ou associado explicitamente aos gerenciadores estáveis da
    sessão (sem um alocador genérico de slots);
  - o relatório lê e calcula a diferença desde a leitura anterior, sem zerar;
  - testes dirigidos para a leitura do relatório durante um incremento.
- **O contador fica ligado só para medição**, com as estatísticas. Estar dentro de uma trava que já é tomada evita outra
  trava, mas não torna grátis a escrita nem a linha de cache. O número mede trocas de transformação, não o custo total do
  escritor.
- **O +1,2 ns (+13%) na escrita já coberta do caminho de 64 B fica aceito para o experimento**, como exceção registrada
  ao requisito de "nenhuma regressão". Não está aprovado para o padrão, e não é necessariamente temporário: com dois
  modos na política, a seleção pode ficar. O A/B separa o custo da seleção do custo do grão comparando três braços: o pai,
  o novo em `legacy-64` e o novo em `experimental-4`.
- **Política:** a identidade `held_write_grain`, com os modos `legacy-64` e `experimental-4` e o log do valor efetivo,
  pode entrar já num commit pequeno. O `EDEN_POLICY` usa o resolvedor único da L4; não nasce um parser isolado aqui.
- **O apelido `EDEN_DIRTY_GRAIN`** hoje aceita valor inválido em silêncio como 64 B (`:27-29`). Ele passa a separar ausência,
  valor válido e erro de configuração (regra 1 da 7.2 do handoff).

