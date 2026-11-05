# Passagem de contexto: protótipo dos contratos de memória e sincronização

Leia este arquivo inteiro antes de começar. Primeira versão escrita em 2026-11-05 sobre `8361adb`; **reescrita em
2026-11-05 depois de o T1 e o T2 terminarem**, para uma sessão nova que vai fazer o **T3**. Branch de trabalho
das sessões anteriores: `claude/elegant-wozniak-c2taum` (a base original é `claude/peaceful-sagan-f9xjx7`).

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
  `main`; criar PR sem ele pedir; citar identificador de modelo em docs ou código; portar commits dos forks
  em bloco; **habilitar qualquer coisa nova por padrão**.
- Commits: terminar com as linhas `Co-Authored-By` e `Claude-Session` que a sessão indicar. Push no branch
  da sessão.
- Afirmações nos docs levam grau de certeza: **verificado** (você leu no código, com `arquivo:linha`),
  **medido** (número de commit/doc, dizer de quem) ou **hipótese**. Não escreva "verificado" sem ter aberto o
  arquivo. Quando ele disser "confirme no código, não confie na minha leitura", abra e cite as linhas.
- Há um revisor externo (outro modelo) que critica os planos. As revisões já incorporadas estão nas tabelas
  "Revisões" dos docs. Mantenha esse formato: o que a crítica disse, se procede no código, o que mudou.
  O revisor já acertou em coisas que a leitura própria deixou passar (validade do cache de transferências,
  premissa do valor host do syncpoint, precisão sobre o consumidor que confia no syncpoint): confira o que
  ele aponta em vez de defender o texto.

## 2. Onde está o raciocínio (leia nesta ordem)

1. `docs/architecture-plan.md`: seções 0, 2 (princípios e **contratos**), 12 (proteção e semântica de falha) e
   13 (critérios de robustez e protótipo); 12.3 tem o que muda nos padrões (`exact-drain`).
2. `docs/gpu-produced-data-plan.md`: seções 0, 2, 4.1 e 4.2 (diagnóstico, casos dirigidos, protocolo),
   **11** (revisão 3: as linhas 1 a 13 resumem tudo o que o harness provou) e **12** (pendência do T4).
3. `docs/gpu-contract-inventory.md` (o T2, **inteiro**): consumidores, produtores e pontos cegos, os quatro
   mecanismos, efeitos do nível High, apresentação, achados F1 a F6, o possível bug latente da 7.1 e a hipótese
   de 64 bytes da 7.2, e a localização dos timestamps (P1).
4. `tools/gpu-contract-harness/README.md`: o modelo, os casos e o que ele **não** cobre.
5. `docs/back-thread-split-plan.md`: seção 7 (invariantes de sincronização) e 8.1 (como foram feitos os
   harnesses TSan antes). Só consulta.

Resumo em seis linhas: o Bananza provavelmente executa command lists com bytes de antes do produtor (GPU) em
Fast; isso não está provado. O 6d3dd9a (releitura de macro para todo jogo de Switch 2) conserta parte, mas tira
as macros da thread da frente do split e pode custar no FE; ele fica até o consumidor novo provar cobertura. A
solução planejada é um contrato de memória (transferências com versão, conclusão própria e validade) e um de
sincronização (dependências identificadas e satisfeitas, não posições no ring), com a regra de visibilidade num
ponto único e trocável. **Nenhum código do emulador foi alterado até aqui**; o harness e os docs são tudo.

## 3. Estado: T1 e T2 feitos

### T1: harness do modelo dos contratos (feito)

`tools/gpu-contract-harness/` (autocontido, g++ puro, nada de `src/`).

| Commit | O que fez |
| --- | --- |
| `0a9ffec` | Primeira versão: `contracts.h`, modelo com frente, trás e GPU em threads reais, os casos dirigidos e os negativos dos mecanismos, `run.sh` com `-O2`, TSan e ASan/UBSan |
| `3884db6` | **Bug real achado pela revisão:** a validade das transferências cobria só as faixas copiadas. `Transfer` ganhou `covered`; casos `stale_empty_transfer` e `stale_two_pages`; negativos **tipados** (`WordMismatch`, `DependencyViolated`...) |
| `61d936e` | Docs: `exact-drain` é a referência e o padrão; a regra por dependências é experimental |
| `622659f` | Dois modos de incremento do host (`host-early`, `host-deferred`), casos `syncpoint_before_gpu`, `syncpoint_trusted_as_ready`, `other_channel_missing_dependency`, **downloads** GPU → memória do jogo (`pending_download_vs_cpu_write` e dois negativos) e a correção do F1 no plano de listas |
| `638dd77` | Escritas seguradas em três etapas (`held_write_vs_pending_download` e a versão protegida), este handoff |
| (sessão do T3, parte 1) | Harness: avisos em blocos de 64 bytes (`ModelKnobs::notice_block_words`) e os casos `held_write_block_clobbers_gpu_bytes` (negativo, `WordMismatch` sempre) e `held_write_block_exact_notices` (positivo), em tabela separada da janela do pop. Planos: requisitos e princípio para o T4 (`docs/gpu-produced-data-plan.md`, 12.1 e 12.2). **Desenho do T3 em `docs/list-diag-design.md`, aguardando aprovação do usuário: nenhum código do emulador foi escrito** |

### T2: inventário (feito)

`docs/gpu-contract-inventory.md`, commit `78b76db`, mais os acréscimos de `622659f` e deste commit (pendência P1,
decisão sobre o acquire legado, 7.1 e 7.2). Tudo com `arquivo:linha`.

### Como rodar o harness

```sh
tools/gpu-contract-harness/run.sh              # 2000 aleatórias + 200 forçadas por caso, regra, modo e escalonamento
tools/gpu-contract-harness/run.sh 300 30       # mais rápido (a tabela inteira leva ~20 s)
HARNESS_OUT=/algum/dir tools/gpu-contract-harness/run.sh
```

O `run.sh` compila três builds (`-O2`; `-fsanitize=thread`; `-fsanitize=address,undefined`, com
`-Wall -Wextra -Werror`) e roda cada um; os de sanitizer rodam um décimo das repetições e param no primeiro
aviso. O binário aceita `--random N --forced N --seed S --case NOME --host early|deferred|both -v`. Na saída,
cada linha é um caso, uma regra e um modo de host; as colunas são as falhas por rodadas em cada escalonamento
(`random`, `back-lags`, `gpu-lags`, `both-lag`, `eager`), e o veredito mostra o tipo de falha encontrado. **Um
negativo só conta se falhar com o tipo que declarou, e em algum escalonamento forçado** (o aleatório pode não
falhar). Um caso "pass" não pode falhar em nenhum.

**Estado do último `run.sh`:** `run.sh` completo (2000 aleatórias + 200 forçadas por escalonamento no `-O2`; 200 + 20 no TSan e no ASan/UBSan), nos dois modos de host e com 26 casos × 3 regras: `ALL EXPECTATIONS MET` nos três builds, sem aviso de sanitizer, **LeakSanitizer ativo e sem vazamentos**, saída 0 ("all three builds met every expectation without sanitizer reports").

## 4. Decisões tomadas desde a primeira versão

1. **`exact-drain` é a regra de visibilidade de referência e o candidato a padrão**, depois de corrigida a
   validade das transferências e validado o protocolo completo (produtores cobertos, cópia ordenada na posição
   da entrada, conclusão na GPU, publicação). O drain sozinho não garante coerência. O custo acima do
   orçamento vira pendência registrada; opções de compatibilidade ficam explícitas e opt-in; `submitted` não
   volta como padrão (`docs/architecture-plan.md`, 12.3).
2. **A regra por dependências (`AfterSatisfiedDependency`) é experimental**, atrás do mesmo ponto trocável
   (`VisibilityRule`). Falha nos casos `no_dependency` e `producer_after_wait_new_page`; só pode substituir o
   `exact-drain` onde uma condição verificável garanta a mesma versão dos bytes.
3. **F1 (premissa corrigida):** `IncrementHost` é o callback de `SignalFence`. Em Fast com fences `Default`
   ele roda **na sinalização** (`fence_manager.h:128-130`); só com fences adiadas (nível High, `Balanced` ou
   `Accurate`) sobe na conclusão. O plano separa **três coisas**: o valor observado (guest e host), o
   processamento do produtor pela trás e a conclusão real na GPU. Um acquire pelo host prova **ordem, não
   conclusão**.
4. **A transferência tem conclusão própria:** a fence da cópia ordenada depois do produtor. Essa garantia não
   pode depender da política de callbacks antecipados (nem do syncpoint, nem do modo de fence). Em
   `host-deferred` o consumidor que confia no syncpoint passa porque a política dá uma garantia mais forte; o
   erro é depender dela sem declarar (caso `syncpoint_trusted_as_ready`).
5. **Produtor de outro canal:** o protocolo precisa **identificar qual trabalho satisfaz a dependência** e
   garantir que ele está antes da cópia no fluxo. Esperar a fence da cópia não resolve uma dependência
   ausente (`other_channel_missing_dependency`). Mudar as fences globalmente para `Accurate` **não** é a
   solução; qualquer mudança de política de fence é medida à parte no FE.
6. **Downloads pendentes (Lichii 3165a2b e 15126fb):** a escrita da CPU cancela o download pendente sobre a
   faixa (`ClearDownload`, `buffer_cache.h:209-216`) e cada download tem o seu conjunto de faixas. Os negativos
   `pending_download_no_cancel` e `pending_download_single_set` falham com `WordMismatch`.
7. **A janela das escritas seguradas (o que o item "confirme no código" concluiu):** uma escrita da CPU numa
   página com dados da GPU vai para a memória do jogo na hora, e o **aviso aos caches fica segurado** até a
   próxima `InvalidateGPUCache` (início da lista, fim de cada `SignalFence`, ou `OnCPURead`). Nesse intervalo o
   **pop assíncrono** da thread de fences (`PopAsyncBuffers`, `buffer_cache.h:703-730`) pode gravar na memória do
   jogo, por cima dos bytes novos, a faixa que ainda está em `async_downloads` (nada a retira antes de o aviso
   ser aplicado: o ramo "segurar" de `OnCPUWrite` não chama `ClearDownload`, `:169-174`); quando o aviso é
   aplicado, o `CachedWriteMemory` lê o guest (bytes velhos) e sobe para o host. **Possível bug latente**:
   ausência de proteção verificada; probabilidade não medida, não reproduzida no emulador
   (`docs/gpu-contract-inventory.md`, 7.1). O `OnCPURead` aplica os avisos antes de um download **síncrono**
   (`gpu.cpp:263-268`), que não protege o pop assíncrono. O harness reproduz a sequência
   (`held_write_vs_pending_download`, negativo, `WordMismatch` em `gpu-lags` e `both-lag`) e modela a direção
   "o pop pula os bytes cobertos por avisos segurados" (`..._protected`, positivo). **Não proponha a correção
   ainda:** as duas direções (o pop respeita os avisos, ou os avisos são aplicados antes do pop) estão como
   pendência do T4 em `docs/gpu-produced-data-plan.md`, seção 12, com a declaração da seção 2.2. Segunda
   hipótese, da granularidade de 64 bytes dos avisos (7.2): lida, não executada.
8. **Acquire de semáforo legado nunca espera** (`continue` em `do/while(false)`, `puller.cpp:187-270`): padrão
   herdado do Eden/yuzu, **fora do escopo** (mudar afeta os jogos de Switch 1); candidato a contador no
   relatório de saúde, sem mudar o comportamento.
9. **Pergunta aberta nova:** "tempo observado pelo jogo" (o controle de resolução dinâmica do Ocarina se guia
   pelo tempo de GPU medido, Lichii 99ac3d5): `docs/architecture-plan.md`, seção 9, pergunta 6; onde este fork
   produz os tempos está em `docs/gpu-contract-inventory.md`, P1. Sem desenho ainda.

## 5. Pontos cegos do inventário que o T3 precisa respeitar

O filtro de páginas do plano (bitmap marcado em `MarkWrittenBuffer`) **não vê** os produtores abaixo
(`docs/gpu-contract-inventory.md`, 3 e 3.1). O diagnóstico do T3 **não pode declarar "sem diferença" por ter
olhado só o filtro**; ele lista os que conseguir identificar e diz o que não cobre.

- **Imagens e render targets, e seus aliases** (`texture_cache.h:2521-2524`, `pipeline_helper.h:557`): só o
  flag `GpuModified` da imagem.
- **Queries e semáforos** (`query_cache.h:285-310`) e o **release de QMD `add`** (`kepler_compute.cpp:169-174`):
  escrevem direto na memória do jogo, em Fast cedo (`fence_manager.h:81-96`).
- **DMA no caminho lento**, `BufferClear` e **uploads inline** (`maxwell_dma.cpp:143-149`,
  `buffer_cache.h:283-304`, `vk_rasterizer.cpp:1220-1269`): escrevem o guest e invalidam.
- **O reset indireto do Bananza feito na CPU** (`vk_rasterizer.cpp:260-332`): substitui um produtor da GPU por
  uma escrita da CPU.
- **Escritas de HLE:** decodificador (`native_video_decoder.cpp:487-488,568-569`), VIC (`vic.cpp:918-919`), vi
  (`shared_buffer_manager.cpp:243-249`).
- **P4 sem dados da GPU na origem** e **P1 além dos bytes realmente escritos** (a marca é do binding inteiro,
  `buffer_cache.h:1259-1261`): ausência de marca e falsos positivos.
- **A janela das escritas seguradas** (decisão 7) e a granularidade de 64 bytes (7.2): o diagnóstico é o lugar
  de **medir** quantas vezes o pop grava sobre um aviso segurado ainda não aplicado.

Outros fatos que o T3 não pode esquecer: em Switch 2 (`UsesNativeTiling`) só o que a CPU leu entra nos downloads
(`buffer_cache_base.h:541-562`); o snapshot de comandos só existe em canais de ABI nativa
(`nvhost_gpu.cpp:395`); o acquire de syncpoint não satisfaz nada hoje (`puller.cpp:126-132`); na frente do
split nunca se consultam caches nem o scheduler (`docs/architecture-plan.md`, 3.3).

## 5.1 Acréscimos do usuário ao T3 (sessão do T3)

Valem para tudo o que o T3 escrever. O desenho está em `docs/list-diag-design.md`.

1. **Dois problemas separados**, com contadores e logs separados (corrigir um não resolve o outro): (a) a
   **janela do pop** (faixa do download, faixa da escrita, ordem escrita, pop, aplicação); (b) os **blocos de 64
   bytes** (endereço, tamanho do bloco, bytes da GPU cobertos).
2. **Observar sem corrigir.** Nada de travas novas que ordenem o pop em relação à aplicação dos avisos, nem
   aplicação antecipada. A leitura do estado dos avisos pela thread de fences não pode ser um `Gather` nem um
   peek no `GPUDirtyMemoryManager` (o `Gather` consome, e o peek não vê o aviso entre o `Gather` e a aplicação):
   livro-razão próprio, sem trava no escritor, com o aviso ligado ao **lote** que o `Gather` retira (`lot_started`/`lot_done`, só do diagnóstico) e o aviso tratado como **registrado, nunca como escrita concluída** (`docs/list-diag-design.md`, 3.2 a 3.6, com o que ele deixa de ver).
3. **Custo:** contadores agregados (no `EDEN_BACK_THREAD_STATS`) e logs limitados (primeiras N, depois
   amostragem). Plano de teste no FE: diagnóstico desligado contra ligado na praça e no palácio (draws/s, com
   as estatísticas de tempo desligadas), além do Bananza (`docs/list-diag-design.md`, 8.3).
4. **Harness:** feito (acima). O positivo com avisos exatos mostra só que a proteção modelada funciona.
5. **Para o T4** (`docs/gpu-produced-data-plan.md`, 12.2): "aplicar os avisos antes do pop" não pode rodar
   uploads e operações do scheduler na thread de fences (donos e ordem de travas); "o pop respeita avisos
   pendentes" exige a proteção publicada antes de os bytes ficarem vulneráveis, sem que retirá-la abra outra
   janela; **uma intercalação encontrada e reproduzida justifica a correção, a medição dimensiona exposição e
   custo, e uma execução sem ocorrências não a invalida.**

**Alcance a manter em todo texto novo:** no harness, as duas perdas de dados foram reproduzidas e as proteções
modeladas passaram; no emulador, há caminhos plausíveis identificados por leitura, sem ocorrência nem
frequência medidas; a correção de produção está em aberto, e o positivo do harness não decide qual mecanismo
integrar.

## 6. Tarefas, em ordem (pare e reporte ao fim de cada uma)

### T3 — Build de diagnóstico de listas (código real, **desligado por padrão**)

**Estado:** harness feito; **desenho proposto (revisão 1, seção 11 do doc) e aguardando aprovação** (`docs/list-diag-design.md`,
decisões na seção 10). Não escreva código do emulador antes dela. Fases propostas: T3a (`pop` e `blk`) e T3b (`list`).

Escopo que o revisor pediu: **pequeno, opt-in, com testes de integração, sem habilitar nada por padrão.**
Não comece sem ler a seção 5 acima e `docs/gpu-produced-data-plan.md`, 4.1.2.

1. **Chave:** `EDEN_LIST_DIAG=1` (padrão desligado; sem a variável o caminho de hoje roda **sem custo
   mensurável**: uma leitura estática de flag por ponto de observação, e nenhuma cópia, buffer ou trava nova).
2. **O que faz (só observa, **não muda o que é executado**):** para cada entrada em página com faixas
   modificadas pela GPU, executada pelo snapshot **ou** pela memória atual (entradas retomadas):
   (a) cópia GPU → buffer privado do diagnóstico, **ordenada na posição da entrada** (pedido enfileirado no ring
   junto com a entrada, gravado pela trás depois de todo trabalho enviado antes), lida de forma assíncrona,
   frames depois, **sem baixar para a memória do jogo e sem mexer em `gpu_modified_ranges`**; (b) comparação das
   palavras usadas com a versão coerente: deslocamento; cabeçalho, dados de método ou parâmetros de macro;
   método e subcanal; tipo e posição do último produtor da faixa; última espera de host antes da entrada;
   (c) **escrita perdida:** produtor executado depois de a frente ler a entrada, na mesma página, mesmo com a
   página não marcada na leitura; (d) pontos cegos da seção 5: listar os que conseguir identificar e dizer o
   que não cobre; (e) **contador da janela das escritas seguradas:** pops que gravam sobre um aviso segurado
   ainda não aplicado (só contar; sem corrigir); (f) log limitado (primeiras N entradas, depois amostragem) e
   contadores por segundo no `EDEN_BACK_THREAD_STATS`.
3. **Interfaces:** reaproveita `tools/gpu-contract-harness/contracts.h` (`Dependency`, `Transfer` com
   `requested`/`covered`/`valid_until`, `VisibilityRule`, `ListFetchConsumer`). Mova para `src/video_core/` só o
   que o diagnóstico usa; o controle `NeverWait` e o `DirectHostRead` do harness **não** vão. O diagnóstico usa
   a conclusão da **própria cópia** (decisão 4), nunca o valor do syncpoint.
4. **Testes de integração (obrigatórios):** em `src/tests/video_core/` (Catch2; já há 31 casos de
   `memory_tracker.cpp` sobre o rastreio real e `pending_download.cpp`), contra as classes **reais** onde der
   (`MemoryTracker`/`word_manager`, o `DmaPusher` com um rasterizador falso, o `RangeSet`), reproduzindo os
   casos dirigidos que o harness já prova no modelo: primeira escrita, retomada dentro da entrada, reuso pela
   CPU, escrita parcial de página (palavras da GPU sobrevivem ao upload: `SynchronizeBuffer` subtrai
   `gpu_modified_ranges`, `buffer_cache.h:1993-2000`), dois produtores, remapeamento, e a janela das escritas
   seguradas. Com `EDEN_LIST_DIAG` desligado, um teste confere que **nada muda** (mesma saída, nenhum contador
   novo incrementado).
5. **Verificação na nuvem:** `g++ -std=c++20 -fsanitize=... ` do harness (`run.sh`) e
   `g++ -std=c++20 -fsyntax-only -Wall` dos arquivos alterados (o método está em `docs/handoff-aggressive.md`,
   "Fluxo de build e teste dele", e em `docs/perf-handoff.md`, "Método de teste usado aqui"). Depois, o
   usuário roda no Bananza e no FE com o diagnóstico ligado.
6. **Declaração da seção 2.2** para o T3: condição nova, **esperas novas (nenhuma)**, **recursos retidos**
   (buffer privado do diagnóstico e as cópias em voo, com teto), **custo sem uso** (a leitura da flag), caminhos
   afetados (a frente do split **não** pode consultar caches; o pedido de cópia vai pelo ring) e o teste.
7. **Pare** e mostre ao usuário o que foi feito, os testes e o que o diagnóstico **não** cobre.

### T4 em diante

Não comece sem o usuário pedir: protótipo ligado no emulador (`EDEN_GPU_LISTS`), espera real do acquire de
syncpoint (dizendo que garantia dá em cada modo de host: ordem do fluxo em Fast, conclusão no adiado), proteções
da seção 12.2, port do 15126fb, **a correção da janela das escritas seguradas** (decisão 7; escolher entre as
duas direções com a medição do T3). Cada um com a declaração da seção 2.2 (esperas novas, recursos retidos,
custo sem uso, caminhos afetados, teste).

## 7. Testes que o usuário pode fazer em casa, em paralelo (sem código novo)

- Bananza, GPU em Fast, `EDEN_GPU_SPLIT=0` em todas, PowerShell novo a cada linha: matriz
  DMA Accuracy Default/Safe × `EDEN_NX2_MACRO_REFRESH=0/1` (`docs/gpu-produced-data-plan.md` 4.1.1).
  Anotar para cada uma: chão invisível, preto ou certo.
- FE na praça (pior ponto), GPU em Fast: `EDEN_NX2_MACRO_REFRESH=0` contra `=1`, mesmo build. Mede o custo
  do 6d3dd9a.
- Os forks (Eglantine, Lichii) desenham o chão do Bananza em Fast?

## 8. Armadilhas conhecidas

- Arquivos com CRLF (por exemplo `texture_cache/texture_cache.h`): edite preservando as quebras de linha e
  confira `git diff --stat`.
- `pkill -f <padrão>` mata o próprio shell se o padrão estiver na linha de comando.
- O servidor do Duck912 (git.eden-emu.dev) é bloqueado na rede da sessão.
- Os forks de referência podem ser clonados só para leitura: `https://github.com/zortos293/lichii` e
  `https://github.com/Djihads80/eglantine` (`GIT_LFS_SKIP_SMUDGE=1 git clone --depth 50 ...`). Eles são
  ~14 FPS contra ~21 daqui no FE: são referência do que o jogo precisa, não de desenho.
- Na frente do split nunca se consultam caches nem o scheduler, nem depois de uma espera parcial
  (`docs/architecture-plan.md` 3.3).
- **Harness:** um escalonamento só pode esconder um erro (`cpu_reuse_no_hold` não falha em `back-lags`); por
  isso cada negativo é conferido em cinco escalonamentos e com tipo de falha declarado. Ao mexer no modelo,
  mantenha a ordem de travas (`guest_mutex_`, `dl_mutex_`, `held_mutex_`; o buffer do host é folha) e nunca
  empurre um comando para a GPU segurando trava: a thread da "GPU" toma as mesmas para aplicar downloads.
- **Sanitizers:** o LeakSanitizer roda no `run.sh` (`detect_leaks=1`) e foi confirmado com um programa que vaza
  de propósito; a rodada dos sanitizers usa um décimo das repetições.
