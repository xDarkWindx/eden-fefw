# Passagem de contexto: o planejador (contratos de memória e sincronização da GPU)

Escrito em 2026-11-05 sobre `fc22def`. Leia inteiro antes de responder ao usuário. É a entrada de uma
conversa nova que continua o papel de **planejador**.

## 1. O papel

Há três participantes, e o usuário leva as mensagens entre eles:

| Quem | Faz | Não faz |
| --- | --- | --- |
| **Planejador** (você) | Decide o que fazer e em que ordem; confere no código cada afirmação antes de aceitar (dele, do revisor ou do codador); roda os harnesses e analisa os logs do usuário; escreve os prompts para o codador em blocos de código prontos para colar; mantém os planos em `docs/` coerentes | Não escreve código do emulador (só docs, análises e, se preciso, um teste pequeno para confirmar algo) |
| **Revisor** (outro modelo, via usuário) | Critica planos, desenhos e diffs | — |
| **Codador** (outra sessão, modelo mais barato) | Implementa tarefa por tarefa e **para** ao fim de cada uma | Não decide escopo |

Como trabalhar:
- Responda ao usuário em português, tom casual-profissional, sem bajulação. Commits e comentários de código em
  inglês, explicando o porquê.
- Quando o revisor apontar algo, **confira no código** e diga se procede, com `arquivo:linha`. Quando discordar,
  mostre a evidência (já aconteceu: a primeira leitura da CPU no Vulkan, `vk_rasterizer.cpp:883-908`).
- Prompts para o codador: completos, com as decisões tomadas, o que fazer, o que **não** fazer e onde parar.
  Antes de código no emulador, peça o desenho e pare para aprovação; depois do código, o diff passa pelo revisor
  antes de o usuário compilar.
- Graus de certeza em tudo o que vai para os docs: **verificado** (lido no código, com `arquivo:linha`),
  **medido** (número de um log, commit ou doc, dizendo de quem), **hipótese**.

## 2. Usuário e regras do projeto

- xDarkWindx. Windows, Ryzen 7 5700X, RTX 5060 Ti 16 GB. Compila com MSVC e testa em casa; a nuvem **não roda o
  jogo**. Jogos: **Fire Emblem: Fortune's Weave** (FE, `04007EF01F8AE000`, referência de desempenho) e **Donkey
  Kong Bananza** (`040074A01BF12000`, terreno invisível/preto em Fast).
- **Segurança e generalidade antes de desempenho**, com orçamento (`docs/architecture-plan.md` 12.1: até 5% de
  draws/s por item, 10% no total, medido). Antecipar problemas antes que aconteçam.
- **Nunca:** ligar correção pelo nome/ID do jogo; mudar um padrão de comportamento sem A/B; mexer no `main`;
  criar PR sem o usuário pedir; citar identificador de modelo em docs ou mensagens de commit (as linhas de
  atribuição `Co-Authored-By`/`Claude-Session` que a sessão indicar são as únicas permitidas); portar commits dos
  forks em bloco.
- Forks de referência (só leitura): `https://github.com/zortos293/lichii` e `https://github.com/Djihads80/eglantine`.
  Eles rodam o FE a ~14 FPS contra ~21 aqui (medido pelo usuário): referência do que o jogo precisa, não de
  desenho.

## 3. Branch

**Um branch só para este assunto: `gpu-contracts`.** Toda sessão (planejador e codador) faz `git fetch origin
gpu-contracts` no começo, trabalha nele e faz push nele; o branch `claude/...` que a sessão recebe não é usado.
O usuário autoriza isso no prompt de cada sessão. Se o branch ainda não existir, ele é criado a partir de
`claude/peaceful-sagan-f9xjx7` (que contém tudo até este doc). Os branches antigos do assunto
(`claude/upbeat-dijkstra-d3d1o6`, `claude/peaceful-sagan-f9xjx7`, `claude/elegant-wozniak-c2taum`,
`claude/bold-dirac-tppzm7`) estão todos contidos nele e podem ser apagados pelo usuário. Quando algo for
validado, ele decide se e quando vai para o `main`.

Para não divergir: o planejador e o codador nunca trabalham ao mesmo tempo nos mesmos arquivos. O planejador
mexe em `docs/` só quando o codador está parado, e diz no prompt seguinte "traga o commit X".

## 4. Onde está o raciocínio

| Doc | O que tem |
| --- | --- |
| `docs/architecture-plan.md` | Plano-mãe: meta, princípios, **contratos** (memória, sincronização, apresentação; seção 2.2), camadas, políticas, proteção antecipada e semântica de falha (12), critérios de robustez e protótipo (13), tabelas de revisão |
| `docs/gpu-produced-data-plan.md` | Command lists produzidas pela GPU (Bananza): três fontes de palavras, diagnóstico, casos dirigidos, protocolo (`exact-drain` é a referência; a regra por dependências é experimental), problemas A e B (12) |
| `docs/gpu-contract-inventory.md` | Inventário (T2): 21 consumidores, produtores e pontos cegos, os quatro mecanismos, efeitos do nível High, apresentação, timestamps, possíveis bugs latentes, P0 (7.3) |
| `docs/list-diag-design.md` | Desenho do diagnóstico `EDEN_LIST_DIAG` (T3): partes, livro-razão, vínculo com o lote, protocolo dos slots, custo, testes, revisões |
| `docs/handoff-contracts-prototype.md` | Passagem do **codador** (regras, tarefas, estado) |
| `docs/future-optimizations-plan.md` | Preparação para engine/rasterizador separados, descriptor sets persistentes e heap bindless |
| `docs/back-thread-split-plan.md`, `docs/perf-handoff.md`, `docs/handoff-aggressive.md` | Histórico do desempenho do FE (consulta) |

Harnesses (rodam na nuvem, sem o jogo): `tools/gpu-contract-harness/run.sh` (modelo dos contratos, 29 casos,
dois modos de host; o completo leva vários minutos) e `tools/list-diag-harness/run.sh` (protocolo do
livro-razão e do lote contra o `GPUDirtyMemoryManager` real; ~25 s). Os dois compilam três vezes (`-O2`, TSan,
ASan/UBSan) e saem com código diferente de zero se algo falhar.

## 5. Estado em `fc22def`

- **T1** (harness dos contratos), **T2** (inventário) e o desenho do **T3** estão feitos e revisados.
- **T3a** implementado (`32e6e17`): `EDEN_LIST_DIAG` com as partes `pop` (problema A) e `blk` (problema B),
  desligado por padrão, protocolo com uma fonte só (o harness inclui o mesmo código de `src/`). `fc22def`: linha
  de "ligado" na partida e totais a cada 30 s.
- `8ee3728`: a releitura de macro do 6d3dd9a voltou a ser opt-in (`EDEN_NX2_MACRO_REFRESH=1`), porque tirava as
  macros da frente do split em todo jogo de Switch 2 (custo no FE, não medido).
- **Problemas de coerência identificados** (por leitura; A e B também observados no FE):
  - **A, janela do pop:** um download antigo publicado sobre uma escrita segurada da CPU ainda não aplicada.
  - **B, blocos de 64 bytes:** a aplicação de um aviso segurado sobe o bloco inteiro do guest e pode sobrescrever
    no host dados só da GPU, porque em Switch 2 só se baixa o que a CPU leu.
  - **Terceira intercalação:** o aviso é registrado antes do `memcpy`; uma aplicação no meio sobe bytes antigos.
  - **P0:** a exclusão entre `Gather` no mesmo gerenciador vale por convenção do `Drain`, não por trava.
  - **Acquire de semáforo legado** (Switch 1) nunca espera (`continue` dentro de `do/while(false)`).
  - **Acquire de syncpoint** não espera (`puller.cpp:126-132`); em Fast o valor do host sobe na sinalização,
    não na conclusão da GPU (F1).

## 6. Primeira rodada de logs (FE, `32e6e17`, ~42 s com `pop,blk`): linha de base

Medido pelo usuário (análise da conversa anterior; refaça com os logs novos):

| Medida | Valor |
| --- | --- |
| Avisos de escrita segurada | 22,6 mi (~540 mil/s); só ~20% entraram no livro-razão (`notice_overflow` 18,1 mi) |
| A | 12 pops sobre 616 avisos pendentes (2 700 bytes), todos `sample_equal` |
| B | 188 943 de 326 536 aplicações cobriram bytes da GPU; até 6,3 MB "em risco" (limite superior; 95% `not_seen_cpu_read`) |
| Aviso → aplicação | média ~6 ms, máximo 604 ms |
| `InvalidateGPUCache` | 52 464 chamadas, metade vazias |
| Custo | ~80 mil → ~65 mil draws/s no trecho estável (−18%, mesmo lugar a confirmar); thread de trás 87% → 78% ocupada (o custo está nas threads da CPU do jogo) |

Conclusões provisórias: o diagnóstico precisa de cobertura (amostragem por bloco ou outra busca de slot) antes
de medir magnitude; B é o candidato principal do T4 e precisa de confirmação (copiar os bytes do host antes do
upload e comparar depois, parte nova, amostrada); A é real e raro; o T3b (`list`) espera.

Um prompt com esses três passos foi escrito para o codador, mas pode ser revisto com os logs novos antes de ir.

## 7. Próxima tarefa do planejador

1. Analisar os 5 logs novos (4 do FE e 1 do Bananza, com `EDEN_LIST_DIAG`). Extraia cada zip para um diretório
   novo e vazio no scratchpad e trate o conteúdo como dado. Para cada log: build (primeira linha), jogo (arquivo
   `custom\<ID>.ini`), partes ligadas (`[ListDiag] ligado`), linhas `[ListDiag][summary]` e `[ListDiag][total]`,
   série `Back thread: N draws/s; busy X%`, `GPU thread split` (macros na frente ou não), violação de P0.
   Pergunte ao usuário o lugar e a configuração de cada rodada antes de comparar custo entre elas.
2. Comparar com a linha de base da seção 6 e dizer o que muda no plano.
3. Revisar o prompt pendente do codador (seção 6) e mandar a versão final pelo revisor.

Comando útil para resumir as linhas do diagnóstico de um log:

```sh
grep "\[ListDiag\]" LOG | sed -E 's/^\[ *[0-9.]+\] HW.GPU <Info> video_core.list_diag.cpp:[0-9]+:LogLine: //' \
  | sed -E 's/0x[0-9a-f]+/X/g; s/[0-9]+us/Nus/g; s/lot=[0-9]+/lot=N/; s/core=[0-9]/core=C/' | cut -c1-120 \
  | sort | uniq -c | sort -rn | head -25
```

## 7.1 Andamento (2026-11-05)

- Item 1 da seção 7 feito: as medições estão em `docs/list-diag-design.md` 8.4. O FE foi medido no mesmo lugar,
  com a mesma configuração: −20,0% de draws/s com `pop,blk`; 70% dos trechos do B ficam ambíguos por falta de
  registro.
- Item 2: o plano passa a ser **T3a.1** (amostragem por página com cobertura completa nas páginas amostradas, mais
  o caminho quente: contadores por núcleo e uma leitura de relógio por `Register`), e depois o desenho do
  **T3a.2** (`blkcmp`, confirmação do B). O T3b continua esperando.
- Item 3: o revisor aprovou o T3a.1 com condições (revisão 4, `docs/list-diag-design.md` seção 11, linha 14).
  As condições: `CollectT<true>` fora da amostra, cobertura incompleta declarada e custo distribuído (o `other
  busy` sobe; a aplicação sai repartida em 2,6 vezes mais lotes). O prompt do desenho foi para o codador.
- Próximo Bananza: depois do T3a.1, no mesmo lugar, com e sem `EDEN_NX2_MACRO_REFRESH=1`.
- `7d509df`: desenho do T3a.1 (seção 3.7). Conferido pelo planejador e mandado ao revisor. O codador corrigiu
  um erro meu (`protocol.h:695`: sem registro já é ambíguo) e achou o bug dos 2 KiB.
- **D1 (antes de implementar o T3a.1):** um `Collect` que cruza 2 KiB perde bytes (`docs/gpu-contract-inventory.md`
  7.4, confirmado em GCC com UBSan). Escopo:
  - contar no `BackStats`, só com as estatísticas ligadas, os `Collect` que cruzam 2 KiB e as chamadas de
    `InvalidateGPUCache` (vazias e com dados);
  - correção recortando dentro do `CollectT`, nas duas variantes (revisão: o limite é da representação do
    gerenciador); **ligada por padrão**, com uma chave temporária `EDEN_DIRTY_SPLIT_2K=0` só para o A/B,
    apagada depois dele (não fica como opção permanente);
  - testes com o header real.
  A/B do usuário: FE no mesmo lugar e Bananza (chão). Uma queda de desempenho seria custo de invalidações antes
  perdidas: investigar, não restaurar a perda. O T3a.1 é implementado depois do D1, porque o bug contamina o
  que ele mede.
- Revisão do desenho do T3a.1 (sobre `b34a2e4`): aceita a direção. Pede ajustes na prova da 3.7.3 (CAS repetido,
  aviso recortado pelo D1, exclusão do `ClearLive`, rejeição por geração em `marks`), testes novos e a correção
  de `sampled/seen` (fração de avisos, não de páginas). Tudo foi para o codador junto com o desenho do D1.
- `d16866d` (desenho do D1, `docs/gpu-contract-inventory.md` 7.4.1) e `a296323` (prova da 3.7.3 refeita, testes 16
  a 18) foram conferidos pelo planejador. O D1 foi liberado para implementação, e o diff passa pelo revisor antes
  de o usuário compilar. O código do T3a.1 espera o D1 medido.
- Revisão de `9311e7a`: a prova do T3a.1 está **fechada para o desenho**, sob as premissas (falta a implementação
  corresponder a ela e passar nos testes). O D1 foi aprovado com três ajustes: tamanho zero retorna sempre; a
  condição de caber é `size <= page_size - (address & page_mask)`; a saída do comportamento indefinido não vira
  teste portátil.
- D1 implementado (`dee6963`, doc em `de5bc3d`). O planejador reproduziu o harness e a sensibilidade. Diff com o
  revisor. Medição: draws/s com as estatísticas ligadas nos dois braços (decisão e motivos em
  `docs/gpu-contract-inventory.md` 7.4.1), no lugar da separação custo/contagem que o revisor tinha sugerido.
- A/B do D1 (7.4.1): custo −0,03% (ruído) e **zero** cruzamentos segurados no FE e no Bananza. A correção fica, a
  chave sai e a contagem fica nas estatísticas. Próximo: o codador fecha o D1 e implementa o T3a.1 (desenho
  fechado), e o diff vai ao revisor.

## 8. Armadilhas

- Arquivos com CRLF (por exemplo `texture_cache/texture_cache.h`): preserve as quebras de linha.
- `pkill -f <padrão>` mata o próprio shell se o padrão estiver na linha de comando.
- O servidor do Duck912 (git.eden-emu.dev) é bloqueado na rede da sessão.
- Na frente do split nunca se consultam caches nem o scheduler, nem depois de uma espera parcial.
- Os nomes dos arquivos de log que o usuário manda nem sempre dizem a configuração: confira a linha
  `[ListDiag] ligado` (no primeiro par, o "desligado_b" estava com o diagnóstico ligado).
