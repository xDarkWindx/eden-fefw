# Passagem de contexto: protótipo dos contratos de memória e sincronização

Leia este arquivo inteiro antes de começar. Escrito em 2026-11-05 sobre `8361adb`
(branch `claude/peaceful-sagan-f9xjx7`). É a entrada para uma sessão nova que vai adiantar testes e,
depois, implementação.

## 1. Usuário e regras

- xDarkWindx. Fala português, tom casual-profissional, sem bajulação. Responda em português; commits e
  comentários de código em inglês, explicando o porquê.
- Windows, Ryzen 7 5700X, RTX 5060 Ti 16 GB. Jogos de referência: **Fire Emblem: Fortune's Weave** (FE,
  benchmark de desempenho) e **Donkey Kong Bananza** (chão do terreno invisível/preto em Fast).
- Ele compila e testa em casa (MSVC); a sessão de nuvem **não roda o jogo**. Seja econômico: cada build dele
  custa tempo.
- **Diretriz central:** segurança e generalidade antes de desempenho. Uma pequena regressão no FE é
  aceitável dentro do orçamento (`docs/architecture-plan.md`, 12.1: até 5% de draws/s por item, 10% no total,
  medido). Antecipar problemas antes que aconteçam.
- **Nunca:** ligar correção pelo nome/ID do jogo; mudar um padrão de comportamento sem A/B dele; mexer no
  `main`; criar PR sem ele pedir; citar identificador de modelo em commits ou docs; portar commits dos forks
  em bloco.
- Commits: terminar com as linhas `Co-Authored-By` e `Claude-Session` que a sessão indicar. Push no branch
  da sessão.
- Afirmações nos docs levam grau de certeza: **verificado** (você leu no código, com `arquivo:linha`),
  **medido** (número de commit/doc, dizer de quem) ou **hipótese**. Não escreva "verificado" sem ter aberto o
  arquivo.
- Há um revisor externo (outro modelo) que critica os planos. As três revisões já incorporadas estão nas
  tabelas "Revisões" dos docs. Mantenha esse formato: o que a crítica disse, se procede no código, o que mudou.

## 2. Onde está o raciocínio (leia nesta ordem)

1. `docs/architecture-plan.md`: seções 0, 2 (princípios e **contratos**), 12 (proteção e semântica de falha)
   e 13 (critérios de robustez e protótipo). É o plano-mãe.
2. `docs/gpu-produced-data-plan.md`: seções 0, 2, 4.1 e 4.2 (diagnóstico, casos dirigidos, protocolo).
3. `docs/back-thread-split-plan.md`: seção 7 (invariantes de sincronização) e 8.1 (como foram feitos os
   harnesses TSan antes). Só consulta.

Resumo em cinco linhas: o Bananza provavelmente executa command lists com bytes de antes do produtor (GPU)
em Fast; isso não está provado. O 6d3dd9a (releitura de macro para todo jogo de Switch 2) conserta parte,
mas tira as macros da thread da frente do split e pode custar no FE; ele fica até o consumidor novo provar
cobertura. A solução planejada é um contrato de memória (transferências com versão e validade) e um de
sincronização (dependências satisfeitas, não posições no ring), com a regra de visibilidade num ponto único
e trocável.

## 3. Tarefas, em ordem (pare e reporte ao fim de cada uma)

### T1 — Harness do modelo dos contratos (sem código do emulador)

**Objetivo:** provar, fora do jogo, que o protocolo da seção 4.2 do plano de listas entrega a versão certa
de cada entrada nos casos dirigidos, sob escalonamentos diferentes das threads, e que trocar a regra de
visibilidade é local.

**Onde:** `tools/gpu-contract-harness/` (novo): `contracts.h` (interfaces), `model.h`/`.cpp` (ring, frente,
trás e GPU falsos), `cases.cpp` (cenários), `run.sh`, `README.md`. Autocontido: nada de `src/` incluído,
para compilar com g++ puro na nuvem.

**Interfaces mínimas (`contracts.h`), espelhando `docs/architecture-plan.md` 2.2:**
- `Dependency { kind (Semaphore, Syncpoint, Wfi, SyncBit), condition, satisfier, timeout, on_timeout (Suspend padrão, Continue, Abort) }`
- `Transfer { ranges, included_producers, version, completion_fence, valid_until }`
- `VisibilityRule`: interface com duas implementações, `AfterSatisfiedDependency` (a hipótese) e
  `ExactDrain` (conservadora). **É o único lugar que decide se uma entrada precisa esperar.**
- `ListFetchConsumer`: usa só as três acima para obter as palavras de uma entrada (snapshot + faixas da
  transferência ordenada na posição da entrada).

**Modelo:** threads reais (frente, trás, "GPU" com atraso aleatório e independente), ring SPSC, memória do
jogo, buffers do host, snapshot na submissão, conclusões sinalizadas antes da hora (como o
`SignalFenceCoalesced` em Fast) e `IncrementGuest` antes de `IncrementHost` para syncpoints.

**Casos (todos de `docs/gpu-produced-data-plan.md` 4.1.4, mais remapeamento):**
primeira escrita; produtor de outro canal com acquire de syncpoint; retomada dentro da entrada; reuso pela
CPU depois de conclusão antecipada; dois produtores na mesma faixa; argumentos pequenos em página nova
(Hyrule, Lichii 9e33027); escrita parcial de página (Lichii b13c4c1); remapeamento do espaço da GPU entre
produtor e consumidor.

**Critérios de pronto:**
1. Cada caso define a versão esperada; o verificador compara palavra a palavra.
2. Com `ExactDrain`, todos passam.
3. Com `AfterSatisfiedDependency`, passam os casos em que o roteiro tem uma dependência satisfeita entre
   produtor e fetch; o caso "produtor sem dependência" **tem** de falhar (teste negativo que prova que o
   verificador pega o erro).
4. Escalonamento: cada caso roda milhares de vezes com atrasos aleatórios **e** com escalonamentos
   determinísticos escolhidos para expor a corrida (aleatório pode não falhar).
5. Sanitizers: `-fsanitize=thread` e, em outra rodada, `-fsanitize=address,undefined`, sem avisos.
6. Timeout de dependência: o caso "valor que nunca chega" termina com o canal suspenso e diagnóstico, não
   com a entrada executada.
7. `README.md` diz como rodar, o que cada caso prova e o que o modelo **não** cobre (por exemplo: os caches
   reais, o rastreio por página do `word_manager.h`, produtores fora do buffer cache).

**Pare** e mostre ao usuário: resultado por caso, o que falhou com cada regra e o que o modelo não cobre.

### T2 — Inventário (documentação, sem código)

Acrescente aos planos, com `arquivo:linha`:
1. **Consumidores** de dados que a GPU produz (busca de listas, parâmetros de macro, leituras FIFO de macro,
   argumentos indiretos, semáforos, leitura da CPU, uploads) e o caminho de cada um hoje.
2. **Produtores** que escrevem memória do jogo: storage buffers de draws e compute, imagens/render targets
   com alias, queries, cópias DMA, escritas de HLE. Quais passam por `MarkWrittenBuffer`.
3. **Efeitos do nível High (Accurate)**: tudo que lê `Settings::IsGPULevelHigh()` ou
   `IsHighAccuracyPart` (ponto de partida: `docs/architecture-plan.md` 3.5).
4. Como este fork trata o "frame à frente" da apresentação (Eglantine 6a22450), se trata.

### T3 — Build de diagnóstico de listas (código real, desligado por padrão)

Só depois de T1 aprovado pelo usuário. `docs/gpu-produced-data-plan.md` 4.1.2: `EDEN_LIST_DIAG=1`, cópia
GPU → buffer privado ordenada na posição da entrada, comparação com as palavras usadas, contadores e log
limitado, conferência de escritas perdidas. Usa as interfaces do T1 (mova `contracts.h` para
`src/video_core/` nessa hora). **Não muda o que é executado.** Verificação na nuvem: `g++ -std=c++20
-fsyntax-only -Wall` dos arquivos alterados (o método está em `docs/handoff-aggressive.md`, "Fluxo de build
e teste dele", e em `docs/perf-handoff.md`, "Método de teste usado aqui"). Depois, o usuário roda no
Bananza e no FE.

### T4 em diante

Não comece sem o usuário pedir: protótipo ligado no emulador (`EDEN_GPU_LISTS`), espera real do acquire de
syncpoint, proteções da seção 12.2, port do 15126fb. Cada um com a declaração da seção 2.2 (esperas novas,
recursos retidos, custo sem uso, caminhos afetados, teste).

## 4. Testes que o usuário pode fazer em casa, em paralelo (sem código novo)

- Bananza, GPU em Fast, `EDEN_GPU_SPLIT=0` em todas, PowerShell novo a cada linha: matriz
  DMA Accuracy Default/Safe × `EDEN_NX2_MACRO_REFRESH=0/1` (`docs/gpu-produced-data-plan.md` 4.1.1).
  Anotar para cada uma: chão invisível, preto ou certo.
- FE na praça (pior ponto), GPU em Fast: `EDEN_NX2_MACRO_REFRESH=0` contra `=1`, mesmo build. Mede o custo
  do 6d3dd9a.
- Os forks (Eglantine, Lichii) desenham o chão do Bananza em Fast?

## 5. Armadilhas conhecidas

- Arquivos com CRLF (por exemplo `texture_cache/texture_cache.h`): edite preservando as quebras de linha e
  confira `git diff --stat`.
- `pkill -f <padrão>` mata o próprio shell se o padrão estiver na linha de comando.
- O servidor do Duck912 (git.eden-emu.dev) é bloqueado na rede da sessão.
- Os forks de referência podem ser clonados só para leitura: `https://github.com/zortos293/lichii` e
  `https://github.com/Djihads80/eglantine` (`GIT_LFS_SKIP_SMUDGE=1 git clone --depth 50 ...`). Eles são
  ~14 FPS contra ~21 daqui no FE: são referência do que o jogo precisa, não de desenho.
- Na frente do split nunca se consultam caches nem o scheduler, nem depois de uma espera parcial
  (`docs/architecture-plan.md` 3.3).
