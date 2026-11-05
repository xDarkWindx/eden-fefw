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
um sanitizer reclamar. O binário aceita `--random N --forced N --seed S --case NOME -v` (`-v` mostra os
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
  escreveu). A GPU só escreve no buffer do host; nada é baixado para a memória do jogo.
- **Snapshot na submissão** (`Submit`), lido pelo mapeamento da hora.
- **Conclusão antes da hora:** `SyncptIncr` sobe o valor do guest na frente (`IncrementGuest`, como o
  `SignalFenceCoalesced` em Fast) e o do host só quando a GPU executa (`IncrementHost`).
- **Filtro:** 1 bit por página de 4 KiB, ligado pela thread de trás quando grava um produtor. O dono (thread
  de trás) guarda as faixas exatas escritas pela GPU e decide as faixas da transferência.
- **Transferência:** a frente passa ao contrato de memória a região inteira da entrada (`covered`, todas as
  palavras físicas no momento da busca); o contrato aplica o filtro e manda copiar só as páginas marcadas
  (`requested`); o pedido entra no ring na posição da entrada; a thread de trás sobe antes as escritas da CPU pendentes nessas faixas (palavra a
  palavra) e grava a cópia; a GPU copia e sinaliza a fence; a frente sobrepõe ao snapshot.
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

Os dois últimos reproduzem um bug da primeira versão do harness, em que a validade cobria só as faixas
copiadas: com `exact-drain` eles falhavam (20/20 no `eager`, 28–37% no aleatório), porque a regra não manda
esperar quando não há produtor na fila e o consumidor reaproveitava a transferência velha.

Negativos dos mecanismos (cada um desliga uma peça e tem de falhar com as duas regras):

| Caso | Peça desligada | Onde falha (tipo esperado) |
| --- | --- | --- |
| `cpu_reuse_no_hold` | segurar escritas da CPU | `gpu-lags`, `eager` e aleatório; **não** em `back-lags` (o upload acontece antes do produtor, que escreve por cima) — `WordMismatch` |
| `partial_page_write_page_upload` | upload por palavra (sobe a página inteira) | sempre — `WordMismatch` |
| `remap_ignore_valid_until` | conferir `valid_until` | sempre — `WordMismatch` |
| `other_channel_guest_value` | acquire de syncpoint pelo valor do host (usa o do guest) | em todos menos `eager` — `DependencyViolated` |

`NeverWait` é um controle do harness, não o comportamento atual do emulador: mostra quais casos dependem
de alguma espera da regra.

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
- **Falhas da seção 12.2** além do limite de espera: exceções na validação e no efeito, guardas no consumo,
  fuzz de listas e `Abort` não são exercitados. A reavaliação depois de `Suspend` existe no código, mas
  nenhum caso a satisfaz depois.
- **Configurações do emulador:** split ligado/desligado, DMA Safe, "Sync Memory Operations" e canais sem ABI
  nativa (que não têm snapshot) não entram.
