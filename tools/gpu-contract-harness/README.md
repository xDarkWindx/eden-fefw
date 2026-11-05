# Harness dos contratos de memória e sincronização (T1)

Modelo, fora do emulador, do protocolo de `docs/gpu-produced-data-plan.md` 4.2 com as interfaces de
`docs/architecture-plan.md` 2.2 e 13.2. Não inclui nada de `src/`: compila com g++ puro.

## Como rodar

```sh
tools/gpu-contract-harness/run.sh            # 2000 aleatórias + 200 forçadas por caso, regra e escalonamento
tools/gpu-contract-harness/run.sh 300 30     # mais rápido
HARNESS_OUT=/algum/dir tools/gpu-contract-harness/run.sh
```

O `run.sh` compila três vezes (`-O2`; `-fsanitize=thread`; `-fsanitize=address,undefined`), com
`-Wall -Wextra -Werror`, e roda cada uma; os builds com sanitizer rodam um décimo das repetições e
param no primeiro aviso. Sai com código diferente de zero se alguma expectativa não for cumprida ou se
um sanitizer reclamar. O binário aceita `--random N --forced N --seed S --case NOME --host early|deferred|both -v` (`-v` mostra os
diagnósticos de suspensão e as escritas perdidas).

## Arquivos

| Arquivo | Conteúdo |
| --- | --- |
| `contracts.h` | `Dependency`, `Transfer`, `MemoryContract`, `VisibilityRule` (com `AfterSatisfiedDependency`, `ExactDrain` e o controle `NeverWait`) e `ListFetchConsumer`. É o que vai para `src/video_core/` no T3. |
| `model.h`, `model.cpp` | Ring SPSC, frente (canais, consumidor, CPU do jogo), thread de trás (filtro de páginas, uploads, cópias ordenadas, conferência de escrita perdida) e "GPU" (único dono do buffer do host). |
| `cases.cpp` | Os casos, com a versão esperada de cada fetch escrita à mão, e o executor. |

## O modelo

- **Threads reais:** a frente roda os programas dos canais e o consumidor; a thread de trás grava os itens
  do ring; a "GPU" executa os comandos gravados com atraso próprio. A CPU do jogo **não** é uma thread: as
  escritas dela acontecem na frente, em pontos fixos do roteiro.
- **Memórias:** memória do jogo (o que a CPU vê e de onde sai o snapshot) e buffer do host (o que a GPU
  escreveu). A GPU escreve no buffer do host e, nos downloads (abaixo), devolve à memória do jogo só o que a
  CPU lê.
- **Snapshot na submissão** (`Submit`), lido pelo mapeamento da hora.
- **Três coisas que o syncpoint mistura** (`docs/gpu-produced-data-plan.md`, 4.2): o valor observado
  (guest e host), o processamento do produtor pela thread de trás e a conclusão real na GPU. O guest sobe na
  frente, na instrução de incremento (`IncrementGuest`, como `SignalFenceCoalesced` em Fast). O host sobe de
  um de dois jeitos, e **todos os casos rodam nos dois** (coluna `host` da tabela, `--host early|deferred`):
  - **`host-early`:** a thread de trás sobe o host quando grava o incremento, antes de a GPU chegar lá. É Fast
    com fences `Default`: `IncrementHost` é o callback do `SignalFence` e roda na sinalização
    (`fence_manager.h:128-130`). O acquire satisfeito prova **ordem** (a trás já gravou tudo que veio antes),
    não conclusão.
  - **`host-deferred`:** a GPU sobe o host quando executa o incremento. É o nível High ou `GPU Fence Behavior`
    `Balanced`/`Accurate`: o callback espera a fence. O acquire satisfeito prova ordem **e** conclusão.
  Nos escalonamentos que atrasam a GPU (`gpu-lags`, `both-lag`) a GPU só é liberada quando algo estacionado
  depende dela (semáforo, syncpoint em `host-deferred`), então em `host-early` o acquire passa com a GPU
  parada; o resultado conta isso (`syncpoint ahead of the GPU`, visível com `-v`).
- **Filtro:** 1 bit por página de 4 KiB, ligado pela thread de trás quando grava um produtor. O dono (thread
  de trás) guarda as faixas exatas escritas pela GPU e decide as faixas da transferência.
- **Transferência:** a frente passa ao contrato de memória a região inteira da entrada (`covered`, todas as
  palavras físicas no momento da busca); o contrato aplica o filtro e manda copiar só as páginas marcadas
  (`requested`); o pedido entra no ring na posição da entrada; a thread de trás sobe antes as escritas da CPU pendentes nessas faixas (palavra a
  palavra) e grava a cópia; a GPU copia e sinaliza a fence; a frente sobrepõe ao snapshot.
- **Downloads GPU → memória do jogo.** O jogo lê faixas (`CpuRead`); num `Fence` a thread de trás enfileira
  um download só das palavras que a GPU escreveu **e** a CPU lê (como nos canais de Switch 2,
  `buffer_cache_base.h:541-562`): a GPU copia para um staging na posição do fluxo e, depois, a fence conclui
  e a aplicação grava na memória do jogo. Escrita da CPU sobre uma faixa **cancela** o que está pendente nela
  (`ClearDownload`, `buffer_cache.h:209-216`) e tira a faixa de `gpu_modified_ranges` (`WriteMemory`,
  `:132-138`). Cada download tem o seu conjunto de palavras vivas (depois do 15126fb do Lichii). As
  conferências são `CheckGuest` (memória do jogo) e `CheckHost` (buffer do host, depois de esvaziar trás e GPU).
  O dono das faixas, o conjunto de downloads e as leituras da CPU ficam sob um mutex (o do buffer cache); a
  ordem de travas é memória do jogo, depois esse; o buffer do host tem mutex próprio.
- **Escritas seguradas em três etapas** (`CpuWriteHeld`, `ApplyHeld`; `docs/gpu-contract-inventory.md`, 7.1).
  Uma escrita da CPU numa página com dados da GPU não passa por `WriteMemory`: ela é segurada
  (`buffer_cache.h:169-174`). O modelo separa o que o emulador separa, cada passo numa seção crítica
  própria:

  | Passo | Quem | Seção crítica | Código real |
  | --- | --- | --- | --- |
  | (i-a) decidir se a página tem dados da GPU | frente | `dl_mutex_` (o mutex do buffer cache), só para decidir | `OnCPUWrite`, `vk_rasterizer.cpp:994-1003`, `buffer_cache.h:160-180` |
  | (i-b) registrar o aviso segurado | frente | `held_mutex_`, sozinho | `GPUDirtyMemoryManager::Collect`, `core/memory.cpp:765`, `core/gpu_dirty_memory_manager.h:26-48` |
  | (i-c) gravar os bytes na memória do jogo | frente | `guest_mutex_`, sozinho | o `memcpy` depois de `GetPointerImpl`, `core/memory.cpp:649-653` |
  | (ii) a thread de fences publica o download (o "pop") | GPU (faz o papel da thread de fences) | `guest_mutex_` + `dl_mutex_` | `PopAsyncBuffers`, `buffer_cache.h:703-730`, chamado de `fence_manager.h:241-263,286-293` |
  | (iii) aplicar o aviso: ler a memória do jogo e subir para o host | trás (item `HeldApply` do ring; também no fim de todo `Fence`, `fence_manager.h:139`) | `guest_mutex_` + `dl_mutex_` + `held_mutex_`, e o upload é empurrado **sem trava** | `InvalidateGPUCache` → `OnCachedWrites` → `CachedWriteMemory`, `gpu.cpp:123-163`, `vk_rasterizer.cpp:958-992`, `buffer_cache.h:141-157,2105-2128` |

  **Nenhuma trava cobre (i)–(iii):** as travas são tomadas e soltas dentro de cada passo; a ordem é
  `guest_mutex_`, depois `dl_mutex_`, depois `held_mutex_` (o buffer do host é folha), e a frente toma
  `held_mutex_` e `guest_mutex_` **sozinhos**, nunca aninhados. A prova de que a janela existe é o próprio
  caso negativo: ele falha em `gpu-lags` e `both-lag` em todas as rodadas (o pop acontece entre (i) e (iii)).
  O roteiro fixa a ordem (i) → (ii) → (iii) com `WaitDownloads` (que libera a GPU e espera o pop) e
  `ApplyHeld`; o escalonamento decide se o pop já tinha acontecido antes de (i): em `eager` sim, e o caso
  passa.
- **Validade:** `valid_until` guarda a época do mapeamento e a soma das épocas de **todas** as páginas de
  `covered`, inclusive as que o filtro descartou (sobem com produtor gravado e escrita da CPU). "A GPU não
  escreveu nada aqui" também é informação e envelhece do mesmo jeito. A validade é lida antes do filtro, e
  a thread de trás liga o bit do filtro antes de subir a época, então um produtor gravado no meio deixa a
  transferência inválida em vez de sumir. O consumidor só reaproveita uma transferência em cache se a regra
  não mandou esperar e ela ainda vale. Produtores enviados e ainda não gravados não mexem na época: cobri-los
  é papel da regra de visibilidade.
- **Escritas da CPU seguradas:** uma escrita sobre uma entrada submetida e ainda não buscada vai para a
  memória do jogo, mas o upload dela para o host fica segurado até a última busca dessa entrada.
- **Dependências:** acquires de semáforo e de syncpoint estacionam o canal (a frente continua os outros) até
  a condição valer; o syncpoint vale pelo valor do host. Ao passar, a frente põe um marco no ring
  (`Dependency::position`). WFI e bit de sync são só marcos. Limite estourado: `Suspend` (padrão: canal
  parado, diagnóstico, a condição continua sendo reavaliada), `Continue` (opção explícita) ou `Abort`.
- **Escrita perdida:** depois de cada busca vai um marco ao ring; a thread de trás confere se algum produtor
  enviado antes escreveu nas palavras da entrada sem estar entre os produtores incluídos, e conta.

**Versão esperada** (definição da 4.2): o snapshot da submissão mais as faixas escritas por todo produtor
enviado ao ring antes da busca, e só por eles, o mais novo por cima. Cada caso escreve essa versão à mão; o
verificador compara palavra a palavra.

## Escalonamentos

| Nome | O que força |
| --- | --- |
| `random` | Atrasos aleatórios e independentes nas três threads. |
| `back-lags` | A thread de trás só grava o que a frente espera explicitamente (janela máxima entre o envio de um produtor e a gravação dele). |
| `gpu-lags` | A GPU só executa o que a frente espera explicitamente (conclusão o mais tarde possível). |
| `both-lag` | Os dois. |
| `eager` | Depois de cada envio a frente espera trás e GPU esvaziarem (nenhuma janela). Mostra o que um escalonamento "bonzinho" esconde. |

Um caso "pass" não pode falhar em nenhum escalonamento. Um negativo declara o **tipo** de falha esperado
(`fail:WordMismatch`, `fail:DependencyViolated`...): tem de falhar em pelo menos um escalonamento forçado
(aleatório pode não falhar), e qualquer falha de outro tipo, em qualquer escalonamento, reprova o teste
("WRONG KIND"). A coluna de veredito mostra os tipos encontrados.

| Tipo | Quando |
| --- | --- |
| `WordMismatch` | uma busca viu palavras diferentes da versão esperada |
| `DependencyViolated` | uma entrada começou antes de a dependência valer, ou executou sem ela |
| `UnexpectedSuspend` | um canal não terminou (ficou suspenso ou parado) sem o caso pedir |
| `MissingSuspend` | um canal que tinha de terminar suspenso não terminou assim |
| `ContinueMismatch` | `on_timeout=Continue` tomado ou não, ao contrário do caso |
| `Timeout` | dependência estourou o limite com `on_timeout=Abort` |
| `Hang` | o watchdog de 20 s expirou |
| `CaseError` | o roteiro é inconsistente |

## Casos e o que cada um prova

| Caso | Roteiro | Prova |
| --- | --- | --- |
| `first_write` | produtor em página nunca marcada, WFI, busca | o produtor na fila entra na versão quando há dependência entre ele e a busca |
| `no_dependency` | produtor e busca sem nada entre eles | **negativo da regra:** `AfterSatisfiedDependency` tem de falhar e falha; `ExactDrain` passa; a escrita perdida é contada |
| `other_channel` | canal 0 espera syncpoint ≥ 1; canal 1 produz e incrementa | a busca só começa com o valor do host satisfeito; o canal à espera não bloqueia o produtor |
| `resume_inside_entry` | metade da entrada antes de um acquire de semáforo, resto depois do release do outro canal | a parte retomada vê o produtor; a primeira parte, o snapshot |
| `cpu_reuse` | conclusão sinalizada antes da hora, o jogo reescreve a memória da entrada antes da busca | a entrada executa a submissão mais o produtor, não a reescrita |
| `two_producers` | P1, WFI, P2 na mesma faixa | vale o mais novo (P2), porque a cópia é ordenada na posição da entrada |
| `producer_after_wait_new_page` | P1, WFI, P2 numa página ainda não marcada | **negativo da regra:** P2 entra na definição, mas a regra confia no filtro depois da espera |
| `small_args_new_page` | 4 palavras de argumentos em página nova, bit de sync | nenhum limiar de tamanho (Lichii 9e33027) |
| `partial_page_write` | GPU escreve 8 palavras, CPU escreve 4 ao lado na mesma página | o upload da CPU não apaga as palavras da GPU, sem download síncrono (Lichii b13c4c1) |
| `remap` | produtor no VA X → página 12; busca; remapeia X → página 13; nova busca em X | a segunda busca vê só a página 13; a transferência em cache deixa de valer |
| `never_arrives` | canal 0 espera syncpoint que nunca chega (limite 2 ms); canal 1 trabalha | canal 0 termina suspenso, com diagnóstico, sem executar a entrada; o canal 1 termina certo |
| `never_arrives_continue` | igual, com `on_timeout = Continue` | a opção de compatibilidade segue sem a dependência e diz isso |
| `stale_empty_transfer` | busca sem dados da GPU (transferência vazia em cache), produtor na mesma página, WFI, nova busca | a ausência de dados também perde a validade |
| `stale_two_pages` | entrada em duas páginas, só uma marcada na primeira busca; produtor na outra, WFI, nova busca | a validade cobre a página que o filtro descartou |
| `syncpoint_before_gpu` | canal 1 produz e incrementa; canal 0 espera o syncpoint e busca. Em `host-early` guest e host alcançam o valor com a GPU atrasada (o resultado exige que isso tenha acontecido em todas as rodadas `gpu-lags` e `both-lag`) | o syncpoint prova ordem, não conclusão: a entrada vê o produtor porque espera a fence **da própria cópia**, ordenada depois do produtor no fluxo |
| `other_channel_missing_dependency` | produtor no canal 1, consumidor no canal 0, **nenhuma** dependência entre eles | esperar a cópia não substitui a dependência: a cópia é ordenada na posição do fetch, e o produtor não está antes dela no fluxo. Falha em todo escalonamento, com as duas regras e nos dois modos (`WordMismatch`). É o caso que o protocolo não pode corrigir sozinho: a dependência tem de existir e o protocolo precisa identificar qual trabalho a satisfaz |
| `pending_download_vs_cpu_write` | P1 escreve 16 palavras que a CPU lê; fence F1 (download D1); a CPU escreve 4 palavras no meio (D1 as perde); P2 escreve as 8 últimas; fence F2 (D2); D1 e D2 concluem; WFI; busca | (Lichii 3165a2b e 15126fb) a escrita nova sobrevive na memória do jogo, o download antigo não a sobrescreve nem consome o novo, e o upload seguinte para o host leva os bytes novos (`CheckGuest`, entrada vista pela busca e `CheckHost`). Ordens adversas: `eager` conclui o download **antes** da escrita; `gpu-lags` e `both-lag` conclui **depois**; o aleatório inclui o meio (entre a cópia para o staging e a aplicação) |
| `held_write_vs_pending_download_protected` | o roteiro do negativo abaixo (`held_write_vs_pending_download`), com o pop pulando as palavras cobertas por avisos segurados ainda não aplicados | **positivo:** passa em todos os escalonamentos, com as duas regras e nos dois modos de host. Nenhum mecanismo do emulador faz isso hoje: o `OnCPURead` aplica as escritas seguradas antes de um download **síncrono** (`gpu.cpp:263-268`), o que não protege o pop assíncrono da thread de fences. A direção alternativa (aplicar os avisos antes do pop) também fecha a janela e fica como pendência do T4 |

Os dois `stale_*` reproduzem um bug da primeira versão do harness, em que a validade cobria só as faixas
copiadas: com `exact-drain` eles falhavam (20/20 no `eager`, 28–37% no aleatório), porque a regra não manda
esperar quando não há produtor na fila e o consumidor reaproveitava a transferência velha.

Negativos dos mecanismos (cada um desliga uma peça e tem de falhar com as duas regras):

| Caso | Peça desligada | Onde falha (tipo esperado) |
| --- | --- | --- |
| `cpu_reuse_no_hold` | segurar escritas da CPU | `gpu-lags`, `eager` e aleatório; **não** em `back-lags` (o upload acontece antes do produtor, que escreve por cima) — `WordMismatch` |
| `partial_page_write_page_upload` | upload por palavra (sobe a página inteira) | sempre — `WordMismatch` |
| `remap_ignore_valid_until` | conferir `valid_until` | sempre — `WordMismatch` |
| `other_channel_guest_value` | acquire de syncpoint pelo valor do host (usa o do guest) | em todos menos `eager` — `DependencyViolated` |
| `syncpoint_trusted_as_ready` | esperar a fence da própria cópia (o consumidor lê o buffer do host logo depois do acquire) | **`host-early`:** aleatório, `gpu-lags` e `both-lag` — `WordMismatch`. **`host-deferred`:** passa (o syncpoint também prova conclusão), por isso a expectativa muda com o modo |
| `pending_download_no_cancel` | cancelar os downloads pendentes antes de a CPU escrever | `gpu-lags`, `both-lag`, aleatório (e parte de `back-lags`) — `WordMismatch`: o download antigo cai por cima da escrita nova. Não em `eager`, onde conclui antes da escrita |
| `pending_download_single_set` | um conjunto único de faixas pendentes para downloads diferentes da mesma faixa (antes do 15126fb) | `gpu-lags`, `both-lag`, aleatório e parte de `back-lags` — `WordMismatch`: o download antigo consome a entrada do novo, que fica sem aplicar e a memória do jogo termina com os bytes velhos |
| `held_write_vs_pending_download` | **o comportamento atual**: download D1 pendente; (i) a CPU escreve 4 palavras (bytes na memória do jogo, aviso segurado); (ii) o pop de D1; (iii) o aviso é aplicado e o upload lê a memória do jogo | `gpu-lags` e `both-lag` em todas as rodadas, mais parte do aleatório e do `back-lags` — `WordMismatch` (a escrita nova se perde na memória do jogo e no host). Não em `eager`, onde o pop vem antes da escrita |

`NeverWait` é um controle do harness, não o comportamento atual do emulador: mostra quais casos dependem
de alguma espera da regra.

## Resultado do último `run.sh`

Preenchido ao fim da rodada que acompanha cada commit; o texto abaixo vale para o commit que o contém.

- Três builds: `-O2` (2000 rodadas aleatórias e 200 forçadas por escalonamento, para cada caso, regra e modo
  de host), `-fsanitize=thread` (200 e 20) e `-fsanitize=address,undefined` (200 e 20, com
  `-fno-sanitize-recover=all`, `detect_leaks=1` e `halt_on_error=1`).
- O **LeakSanitizer rodou**: com as mesmas opções ele reporta um vazamento num programa de teste que perde
  64 bytes de propósito (saída 1, `LeakSanitizer: detected memory leaks`), então o silêncio na rodada do
  harness quer dizer "sem vazamentos".
- Estado: ver o fim deste arquivo (linha "Último run.sh").

**Precisão sobre `syncpoint_trusted_as_ready` (revisão):** em `host-deferred` o consumidor que lê o buffer do
host logo depois do acquire **passa**, e passa porque a política de fence adiada dá uma garantia mais forte
(o valor do host só sobe quando a GPU conclui). O erro do consumidor não é falhar nesse modo: é **depender
dessa garantia sem declará-la**. Em `host-early` (Fast com fences `Default`) a mesma leitura falha. Por isso o
caso tem expectativas diferentes por modo, e a regra do protocolo é que a conclusão vem da fence da própria
cópia, não da política de callbacks.

## O que o modelo não cobre

- **Os caches reais.** Não há `BufferCache`, `TextureCache`, `QueryCache` nem o rastreio por página de
  `word_manager.h`; o "dono" é um vetor de palavras escritas pela GPU. Bugs de faixas, de alinhamento, de
  limpeza de bits e de sobreposição entre buffers ficam fora.
- **Produtores fora do buffer cache:** imagens e render targets com alias, storage images, queries,
  escritas de HLE e cópias DMA não marcam nada aqui, como também não marcariam o bitmap real (inventário do
  T2).
- **`DmaPusher` e puller reais.** Não há parse de métodos, lista de prefetch, `command_resume_offset` no
  meio de um método, parâmetros de macro, leituras FIFO de macro nem argumentos indiretos. Uma "entrada" é
  só um bloco de palavras buscado em um ou dois pedaços.
- **A CPU do jogo como thread independente.** As escritas e observações da CPU acontecem em pontos fixos
  do roteiro da frente. Uma escrita da CPU concorrente com o snapshot ou com a própria busca não é exercitada.
- **Ordem de memória da GPU real.** O modelo supõe que a cópia gravada depois de um produtor no mesmo fluxo
  vê o que ele escreveu (barreira certa). Barreiras do Vulkan, várias filas e caches da GPU não são modelados.
- **Tradução de endereço:** o VA de um produtor é resolvido quando a frente o envia; uma entrada remapeada
  entre a submissão e a busca não é coberta (o snapshot e a busca usariam mapeamentos diferentes).
- **Filtro nunca limpo:** os bits só sobem, então falsos positivos e o custo deles não aparecem. O modelo
  não mede custo nenhum.
- **Escritas seguradas simplificadas:** segura-se o upload de qualquer escrita da CPU sobre uma entrada
  submetida e não buscada, sem limite de memória nem de tempo, e sem a junção por página do mecanismo real.
- **Outros clientes do host1x**, incrementos fora de ordem e syncpoints que dão a volta.
- **Dependência identificada, não só satisfeita.** O modelo exige que o roteiro tenha a dependência entre o
  produtor e a busca; ele não decide qual trabalho a satisfaz. Esse é o requisito do protocolo
  (`other_channel_missing_dependency` só mostra o que acontece sem ela). Para produtores de outro canal, o
  protocolo real precisa associar a dependência ao trabalho que a satisfaz.
- **Downloads simplificados:** um fence enfileira um download por vez de todas as palavras GPU-escritas e
  lidas pela CPU, e a aplicação é uma etapa só na thread da "GPU"; não há `async_downloads`, junção de
  buffers, preflush, `cpu_read_ranges` por página, nem a fila de fences com a thread própria
  (`fence_manager.h:241-272`). A escrita comum cancela o pendente **na hora**, que é o que o emulador faz
  quando a página não tem dados da GPU; a escrita em página com dados da GPU é segurada e tem os três passos
  separados nos casos `held_write_*` (acima). Esses casos usam o aviso exato em palavras; o emulador junta os
  avisos em granularidade de **64 bytes** (`core/gpu_dirty_memory_manager.h:90-96,113-121`), e o upload da
  aplicação copia o trecho arredondado (`buffer_cache.h:141-157`): bytes vizinhos da GPU dentro do mesmo trecho
  de 64 bytes não são modelados (hipótese em `gpu-contract-inventory.md`, 7.2).
- **O pop não tira as faixas de `gpu_modified_ranges`** (`buffer_cache.h:723-725`) nem usa o conjunto
  contado de `async_downloads`; a aplicação dos avisos sempre faz o upload exato dos bytes, sem o caminho
  `WriteMemory` (que marcaria a página para o upload seguinte; os bytes lidos são os mesmos).
- **Os modos de fence são dois, não a política inteira.** `host-early` e `host-deferred` cobrem só quando o
  valor do host sobe; `Balanced`/`Accurate` também mudam `runtime.Finish()` em DMA e a sincronização em
  `FindBuffer`, que o modelo não tem.
- **Falhas da seção 12.2** além do limite de espera: exceções na validação e no efeito, guardas no consumo,
  fuzz de listas e `Abort` não são exercitados. A reavaliação depois de `Suspend` existe no código, mas
  nenhum caso a satisfaz depois.
- **Configurações do emulador:** split ligado/desligado, DMA Safe, "Sync Memory Operations" e canais sem ABI
  nativa (que não têm snapshot) não entram.

**Último run.sh:** `run.sh` completo (2000 aleatórias + 200 forçadas por escalonamento no `-O2`; 200 + 20 no TSan e no ASan/UBSan), nos dois modos de host e com 26 casos × 3 regras: `ALL EXPECTATIONS MET` nos três builds, sem aviso de sanitizer, **LeakSanitizer ativo e sem vazamentos**, saída 0 ("all three builds met every expectation without sanitizer reports").
