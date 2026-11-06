# Desenho da etapa 3: B-split redesenhado contra heap bindless (Fortune's Weave)

Escrito em 2026-11-04 sobre o `main` em `740b932003` (release 1.12). Para revisão antes de qualquer código.
**Revisão 2:** incorpora a revisão adversarial (seção 10); as seções 2-8 foram corrigidas no lugar. Números
das rodadas 1-14 de `docs/back-thread-split-plan.md` (seções 3.7-3.22), na praça da cidade, RTX 5060 Ti, 1x.

## 1. Onde estamos e o que falta

- **Pior ponto da praça (o escolhido para medir): 20-21 FPS** há várias rodadas. A thread de trás trabalha ali
  ~43-45 ms por frame (~3.700 draws a ~90% ocupada). Outros trechos melhoraram mais (um foi de 22 para 30, a vista
  aberta comparada com a 1.10 de 16 para 25), mas o pior ponto ainda não cruza o degrau seguinte.
- A tela emulada só aceita 33,3 ms (30 FPS) ou 50 ms (20 FPS) por frame; médias entre os dois saem como mistura.
- A thread de trás da GPU (`GpuSplit::BackLoop`) é o limite. Para 30 FPS estáveis no pior ponto, o trabalho dela
  por frame precisa cair **~25-27%** (de ~44 para menos de 33 ms). A GPU real, hoje ~67% a 20 FPS, iria a ~100% a 30
  FPS na resolução atual: ver 6.
- Distribuição medida (pior ponto, % do tempo de parede, ~90% ocupada; rodadas 12-14):

| Bloco | % | Origem |
| --- | --- | --- |
| Preparação de draws (`PrepareDraw`) | ~60 | handles ~10, push images ~7,5, stage buffers ~7, buffers ~6-8, flush caching ~8, resolve ~4, lock (esperar + soltar) ~7, resto ~10 |
| Trabalho próprio da engine (métodos, escritas de macro já sem as redundantes) | ~13 | 3,6 mi escritas de macro/s, ~48 por draw |
| Compute, upload inline | ~8 | engine |
| Início/fim de lista, fences, composite | ~2-3 | rasterizador |
| Ociosa | ~7-10 | pacing da tela e esperas pelo jogo |

As micro-otimizações renderam ~1-8% cada e estão perto do fim: o que sobra está espalhado em pedaços de 2-8%. Os
~25-27% que faltam só vêm de tirar um bloco inteiro da thread de trás. Dois caminhos: (B) mover o rasterizador para
outra thread, (C) parar de montar descritores de banco na CPU. Um terceiro, menor (C0), aparece no meio.

## 2. Opção B: segundo split (engine | rasterizador)

### 2.1 Forma

```
front (parsing, macros) ──ring 1──▶ engine (métodos, registradores, DrawManager) ──ring 2──▶ rasterizador (PrepareDraw, caches, scheduler, queries, fences) ──▶ worker Vulkan
```

- A thread **engine** executa `CallMethod`/`CallMultiMethod`, mantém `Maxwell3D::regs`, as flags de sujeira e o
  `DrawManager`, e transforma cada chamada de `RasterizerInterface` (`Draw`, `DrawIndirect`, `Clear`,
  `DispatchCompute`, `Query`, `SignalFence`, `SignalSyncPoint`, `FlushRegion`, `AccelerateDMA`, uploads inline,
  `BindChannel`, ...) num item do ring 2.
- A thread **rasterizador** é a única dona dos caches, do scheduler, do query cache e do fence manager (como a de
  trás é hoje). Ela precisa de uma **cópia própria dos registradores e do estado de draw** no ponto de cada item,
  porque o rasterizador lê `maxwell3d->regs`, `dirty.flags` e `draw_manager.draw_state` em todo lugar.

### 2.2 O que precisa viajar em ordem no ring 2

1. **Escritas de registrador.** Log de deltas (endereço, valor), como a frente já faz para a de trás (`FrontWrite`).
   O rasterizador aplica num `Maxwell3D` sombra e marca as mesmas flags. Custo: ~3,6 mi escritas/s repetidas do
   lado do rasterizador (estimado em 4-7% do tempo atual da thread de trás; **a medir**, portão G2).
2. **Escritas de memória do guest entre draws** (achado da revisão): `ProcessCBMultiData` → `WriteBlockCached`
   grava os bytes do constant buffer **na hora** (`maxwell_3d.cpp:702-715`, `memory_manager.cpp:492-495`), e o
   `Configure` do draw lê essa memória (handles de textura, `FlushCaching`). Com o rasterizador atrasado, o draw A
   leria os bytes do draw B. Opções:
   - **(a) adiar a escrita:** a engine não grava; manda (endereço, bytes) no ring 2 e o rasterizador grava e
     acumula. Problema: tudo que a engine lê dessa memória depois (macros que leem memória, `ReadMacroMemory`, que
     já drenam; DMA; leituras da CPU do jogo) passa a precisar esperar o rasterizador.
   - **(b) versionar:** a engine grava, e o item de draw leva uma cópia das janelas de constant buffer que o draw
     lê. Caro: os bancos de handles têm ~3.000 entradas; copiar por draw desfaz parte do ganho.
   - **(c) sincronizar antes de sobrescrever:** a engine espera o rasterizador passar do último draw que lê aquela
     região. Precisa saber quais regiões cada draw lê; é a (a) com outra contabilidade.
   A (a) é a única que não paga por draw. O mesmo vale para uploads inline (`AccelerateInlineToMemory`) e para o
   que mais escrever memória do lado da engine (DMA, `MaxwellDMA`, `KeplerMemory`).
3. **A fronteira do draw adiado:** `ProcessDirtyRegisters` emite o draw instanciado adiado antes de aplicar a
   escrita (`maxwell_3d.cpp:344-353`). O log precisa preservar essa ordem exata (o draw adiado vira um item antes
   da escrita que o disparou).
4. **Estado de canal:** `BindChannel`, `gpu_memory`, `kepler_compute`, troca de engine.

### 2.3 Sincronização

- Os **~780 acquires/s** do jogo hoje drenam a thread de trás (o release vem de QMD de compute, gravado por um
  callback que roda na hora no modo de fence padrão, `fence_manager.h:80-89`). Com B, o QMD é escalonado na engine
  e o callback roda no rasterizador (`SignalFenceCoalesced`): o acquire teria de drenar **os dois estágios**. Cada
  `Drain` passa a esvaziar um pipeline mais fundo. Alternativa a estudar: rodar o callback de soma na engine
  (é escrita de memória da CPU) e só a fence no rasterizador; precisa provar que nada do rasterizador depende da
  ordem dessa escrita.
- O `Finish` por frame da tabela de handles (`ConfigureImpl` → `DownloadMemory`) continua no rasterizador.
- Leituras da CPU do jogo (`FlushRegion`, `GetFlushArea`), fences, `SignalSyncPoint`, present: continuam no
  rasterizador, na ordem do ring; quem espera resultado (semáforos, DMA que lê memória escrita pela GPU) drena até
  o rasterizador.
- Lifetime: mesma conta da 5.2 do plano (8 fatias, pool de 7 frames); o rasterizador continua sendo o produtor
  único do scheduler, então nada muda ali.

### 2.4 Ganho estimado

Vazão = 1 / max(estágio). Em unidades de tempo de parede, a thread de trás hoje está ~90 de 100 ocupada: engine
~13, compute/inline ~8, rasterizador ~65, outros ~4. Com B:
- engine: ~13 + a parte de engine de compute/inline + envio ao ring 2 (~2-3) ≈ **18-22**;
- rasterizador: ~65 + replicação de registradores (4-7, **a medir**, portão G2) + escritas de memória adiadas (~1-2)
  + a parte de cache de compute/inline (~5) ≈ **75-79**.

Ganho ideal sobre o trabalho ocupado: 90/79 ≈ **×1,14** a 90/75 ≈ **×1,20**, antes das bolhas dos ~780 `Drain`/s em
pipeline mais fundo (2-5%) e de mudanças de disputa de lock. Sobre ~44 ms de trabalho no pior ponto: **~37-39 ms**,
acima dos 33 ms. Os baldes incluem esperas, então isto é uma ordem de grandeza, não uma previsão.

O Maxwell3D sombra do rasterizador aplica os deltas sem executar métodos: em particular, não pode emitir de novo o
draw instanciado adiado que a engine já emitiu (o item do draw adiado vai no ring, na ordem; o sombra só copia o
estado).

### 2.5 Riscos e custo

- **Alto e global.** Toca a engine inteira, o rasterizador inteiro e todas as chamadas de `RasterizerInterface`.
  Erros de ordem aparecem como artefatos intermitentes (dados de um draw no outro), difíceis de reproduzir.
- Deadlocks novos com dois níveis de `Drain` e canais suspensos.
- O ganho depende de G2 (custo de replicar) e de G3 (profundidade de drenagem).
- Leituras da engine sobre memória que ela escreve: além das macros (que já drenam), o caminho de QMD lê
  descritores com `MemoryManager::ReadBlock`, que pode entrar no rasterizador para coerência; mudanças de canal e
  leituras da CPU do jogo também. Adiar escritas (2.2 a) exige ordenar todas essas leituras. O harness precisa de
  casos de leitura depois de escrita entre engines.
- Esforço: **semanas**. Interruptor obrigatório (`EDEN_GPU_SPLIT=3`, por exemplo). Harness com TSan do protocolo de
  dois rings e das escritas de memória adiadas antes do primeiro build do usuário.

## 3. Opção C: heap bindless para os bancos de textura

### 3.1 Forma

O problema que sobra é o próprio banco: ~7 mil draws/s ligam um array de ~2.950 entradas (lidas de um constant
buffer ou de um buffer escrito pela GPU), que a CPU decodifica (`handles`), resolve (`resolve`) e transforma em
descritores (`push images`) a cada draw, mesmo reaproveitando 99% dos bancos. No hardware, o shader lê o handle e
indexa a tabela TIC/TSC; a CPU não faz nada por draw.

C faz o mesmo **só para os arrays de banco** (os descritores com `buffer_source` e os arrays de constant buffer com
16+ elementos que hoje viram tabela compacta); texturas comuns continuam no caminho atual:

- **Heap persistente por tabela TIC:** um descriptor set (ou buffer) global com um array de imagens por tipo de view
  (2D, 2D array, cubo, 3D, ...) indexado pelo **índice TIC**, e samplers por **(TSC, variante)**. O sampler Vulkan
  não depende só do TSC: `Sampler::MakeKey` (`vk_texture_cache.cpp:2902`) escolhe a variante pela imagem do TIC
  (anisotropia suportada, nearest forçado em formato inteiro, redução min/max, border color, sRGB, swizzle; o
  `is_depth` nem entra). O heap precisa de um array de samplers por variante e de uma tabela de tradução por TIC
  que o shader consulte (índice TSC + variante da imagem), ou o caminho otimizado fica restrito aos pares
  comprovadamente equivalentes (portão C-1). Entradas inválidas, de tipo incompatível ou de formato inteiro onde o shader espera float apontam para a
  view nula do tipo (como `PushImageDescriptors` faz hoje).
- **Recompilador:** para esses descritores, em vez de `array[elemento]`, o shader lê o handle no constant buffer (já
  é um UBO ligado) ou no storage buffer, extrai TIC e TSC (com `via_header_index`, segundo handle, exclusões) e
  indexa o heap (`nonuniformEXT`). É o que o recompilador já faz para descobrir o índice do elemento, só que o
  resultado indexa o heap em vez do array do draw.
- **Cache de texturas:** mantém o heap em dia com a tabela TIC do canal. Dois problemas abertos:
  - **Versões do heap.** Atualizar um slot (update-after-bind ou não) não cria snapshot: se o draw A usa o TIC 10,
    a entrada muda e o draw B usa o mesmo slot, A pode ver a versão de B, tanto em comandos gravados ainda não
    submetidos quanto em trabalho na GPU. Trocar por view nula antes de destruir também não protege A. O protocolo
    precisa de versões imutáveis (slots novos por versão, com reaproveitamento só depois do tick da GPU) ou de
    sincronização de reuso, cobrindo mudanças dentro do mesmo frame (portão C-2).
  - **Preparação.** O reaproveitamento atual não é só "comparar uma geração": `TryReuseViewBank`
    (`texture_cache.h:2650-2690`) confere canal, endereço e limite da tabela e o snapshot da memória, e mesmo no
    acerto renova o LRU das imagens (uma vez por frame) e chama `PrepareImage` nas imagens com aliases **a cada
    draw**. Se a CPU deixar de ler os handles, ela precisa de um **conjunto conservador** dos TICs que o shader pode
    acessar para preparar uploads, layouts, aliases e feedback loops; preparar a tabela inteira pode custar mais que
    preparar o banco usado. Não há garantia, neste desenho, de eliminar todo o trabalho por draw.

### 3.2 O que sai da thread de trás

Por draw com banco: decodificar os ~2.950 handles e o `Build` da tabela compacta (parte de `handles`), resolver
views e samplers (`resolve`), montar e copiar descritores (`push images`, ~6 KB copiados e ~6,5 KB guardados por
draw), a comparação do payload e, no worker, o `UpdateDescriptorSet` de ~3.000 entradas. E **o `Finish` por frame
da tabela de handles**: o shader leria a tabela escrita pela GPU direto do buffer (bytes iguais em 100% dos
downloads medidos, rodada 11), sem a CPU precisar dela.

Fica: a manutenção do banco por draw que existe hoje mesmo no acerto (aliases, LRU, conferência da tabela), a do
heap (versões, variantes de sampler) e a preparação de um conjunto conservador de TICs.

### 3.3 Ganho estimado

Se os draws com banco forem a maior parte de `handles` (~10%), `push images` (~7,5%), `resolve` (~4%) e da
comparação/cópia de payload (~1,5%): **~18-23% da thread de trás**, mais o `Finish` por frame (~1-2%). **Portão G1:
medir essa fração** (os blocos hoje não separam draws com e sem banco). Resultado provável: **×1,2-1,3**, sem pipeline
mais fundo nem bolhas novas, **descontada** a manutenção que fica (acima). Ordem de grandeza: ~×1,15-1,25; no pior
ponto, ~35-38 ms, acima dos 33 ms sozinho. O lock dos caches também fica menos tempo seguro (menos disputa com as threads do
jogo).

Custo do lado da GPU: indexação não uniforme custa algo nos shaders desses draws; na NVIDIA costuma ser pequeno,
mas soma à GPU que já vai a ~85-100% a 30 FPS (ver 6). **Portão G4.**

### 3.4 Riscos e custo

- **Alto, mas local:** recompilador (só os descritores de banco), layout de pipeline (um set global a mais), cache de
  texturas (heap). Pode ficar atrás de interruptor por pipeline, com o caminho atual como fallback.
- **Recursos do dispositivo:** o fork desliga `runtimeDescriptorArray`, `UpdateAfterBind` e
  `UpdateUnusedWhilePending` (`vulkan_device.cpp:704-722`). Um heap atualizado entre draws precisa de
  update-after-bind (ou de trocar o set ligado quando muda, com versões por frame). Arrays de tamanho fixo (o
  limite da tabela TIC) evitam `runtimeDescriptorArray`. Conferir os limites da RTX
  (`maxPerStageDescriptorUpdateAfterBindSampledImages` etc.).
- **Portões de correção antes do protótipo:** C-1 (reproduzir todas as variantes de sampler) e C-2 (versões do heap
  entre draws).
- **Semântica:** escala de resolução por imagem (pode virar metadado por entrada do heap) (`rescaling.PushTexture`) não chega ao shader por entrada; com o
  usuário em 1x isso não pesa, mas precisa de regra (heap desligado com escala, ou flag por entrada num buffer).
  Formatos reinterpretados pela TIC, imagens com aliases e render targets amostrados (feedback loop) precisam do
  mesmo tratamento que `PrepareImageView`/`CheckFeedbackLoop` dão hoje, mas para o banco inteiro.
- **Validação:** comparação de imagens (captura) com e sem o heap, nos pontos fixos; validation layers.
- Esforço: **semanas**, concentrado no recompilador e no cache de texturas.

## 4. Opção C0 (intermediária): set de descritores persistente por banco

Sem mudar a semântica dos shaders: o array do banco vai para um **descriptor set próprio** (set 1), montado uma vez
por banco e religado enquanto a geração do banco, os carimbos e a geração de views não mudam; o set por draw (set 0)
fica só com buffers e texturas comuns.

- Sai: `push images` do banco (cópia de ~6 KB por draw), comparação/`assign` do payload e o `UpdateDescriptorSet` de
  ~3.000 entradas no worker. Fica: `handles` (decodificação e `Build`), `resolve` (já com bancos reaproveitados) e o
  `Finish` da tabela.
- Ganho estimado: **~8-11%** (portão G1 também).
- **Allocator dedicado (pergunta 4 da revisão 1):** `ResourcePool::CommitResource` (`vk_resource_pool.cpp:17`)
  pega *qualquer* recurso livre e marca o tick; não existe "renovar o uso deste set", e chamar `Commit()` a cada uso
  não protege o set antigo. C0 precisa de um allocator de sets de banco com:
  - identidade do set (o banco que ele representa: hash dos TICs, carimbos dos bancos de views e samplers, geração
    de views, layout);
  - último uso = o `CurrentTick` do scheduler no momento em que o bind é gravado (cobre os usos ainda na fila do
    worker: aquele tick só é sinalizado depois da submissão e da conclusão na GPU);
  - aposentadoria quando o banco é invalidado (o set sai da tabela de identidade e só volta ao pool quando a GPU
    passa do último uso);
  - proibição de sobrescrever um set com consumidores pendentes (um banco novo sempre pega um set livre, nunca
    atualiza um em uso).
- O conteúdo do set é o mesmo `SampledImage` (view + sampler pela variante da imagem) que `PushImageDescriptors`
  escreve hoje, então as variantes de sampler ficam corretas por construção. A manutenção por draw do banco
  (aliases, LRU) continua.
- Outros riscos: recompilador e layout de pipeline com dois sets (moderado, sem mudar o que o shader lê).
- Esforço: **dias**. Não chega sozinho aos 30 FPS no pior ponto, mas é um passo do caminho C (o set do banco é o
  embrião do heap).

## 5. Comparação

| | B (segundo split) | C (heap bindless) | C0 (set por banco) |
| --- | --- | --- | --- |
| Ganho provável (ordem de grandeza) | ×1,14-1,20 antes de bolhas | ×1,15-1,25, descontada a manutenção | ×1,08-1,12 |
| Chega a 30 no pior ponto? | sozinho, não (~37-39 ms) | sozinho, não (~35-38 ms) | não |
| Onde mexe | engine + rasterizador inteiros | recompilador + cache de texturas (só bancos) | layout de pipeline + descritores de banco |
| Risco | alto, global (ordem, deadlock) | alto, local (shader, heap, recursos do dispositivo) | moderado |
| Esforço | semanas | semanas | dias |
| Fallback | `EDEN_GPU_SPLIT` | por pipeline | por pipeline |
| Efeito colateral bom | divide também a engine | remove o `Finish` por frame e reduz o lock | reduz o worker |
| Custo na GPU | nenhum | indexação não uniforme | nenhum |

## 6. Condições que valem para qualquer opção

- **GPU real:** o teste de resolução do usuário sustenta que o limite hoje é a CPU na configuração habitual. "67% ×
  1,5" é só um cenário, não uma previsão: medir a ocupação da GPU no pior ponto (portão G4) antes de contar com ela.
- Tudo atrás de interruptor, desligado até passar nos pontos fixos (praça, palácio, seleção de personagem, batalha).
- O usuário compila no Windows/MSVC com avisos como erro; cada rodada dele precisa trazer uma mudança candidata a
  padrão ou responder um portão.

## 7. Portões antes de escolher (um build de medição)

| Portão | Pergunta | Decide |
| --- | --- | --- |
| G1 | Que fração de `handles`, `resolve`, `push images` e da comparação de payload vem dos draws com banco? | Se ≥ 15%: C ou C0 valem; se < 10%: C perde a vantagem |
| G2 | Quanto custaria replicar as escritas de registrador do lado do rasterizador? (tempo de `ProcessDirtyRegisters` + tabelas de sujeira por escrita, hoje dentro de "macro writes") | Se > 8%: B perde a vantagem |
| G3 | Quantos itens o ring 2 teria entre um acquire e o release que ele espera (profundidade a drenar)? | Bolha de B |
| G4 | Ocupação da GPU real no pior ponto (perfil de GPU ou `EDEN_GPU_PROFILE`) | Se a GPU já passa de ~80%: qualquer opção precisa de resolução menor para os 30 |

## 8. Recomendação (a criticar)

Nenhuma das opções, sozinha, leva o pior ponto (20-21 FPS, ~44 ms de trabalho por frame) a 30 estáveis. Os ganhos
de B e C **não se multiplicam**: C tira trabalho justamente do rasterizador que B separaria. Depois de C, o tempo de B
é `max(engine, rasterizador) + comunicação e dependências`, recalculado com os estágios novos; a conta "44 / (1,25 ×
1,25)" da revisão 1 era só ilustrativa. Fora do pior ponto, qualquer uma já põe mais trechos em 30.


1. Rodar o build de medição do portão G1 (e G4).
2. Escrever o desenho do allocator de C0 (seção 4) e revisá-lo; se G1 der o esperado (≥ 15%), implementar **C0**
   atrás de interruptor, medir.
3. **C** só depois dos portões de correção C-1 (variantes de sampler) e C-2 (versões do heap) resolvidos no papel.
4. **B** só depois de medir a partição exclusiva engine/rasterizador (G2, G3) e com o caso de leitura depois de
   escrita entre engines no harness.

## 9. Perguntas para o revisor

1. A estimativa de replicação de registradores em B (4-7%) é razoável, dado que cada escrita marca 2 tabelas de
   sujeira e pode emitir o draw adiado?
2. Para B, existe alguma leitura da engine sobre memória que ela mesma escreve entre draws (além das macros que já
   drenam) que tornaria a opção (a) de 2.2 inviável?
3. Para C, como tratar escala de resolução e feedback loops por entrada do heap sem voltar a ter trabalho por draw?
4. Para C0, o reaproveitamento do set entre frames é seguro com o `ResourcePool` atual se o set for "tocado" a cada
   uso, ou precisa de um pool próprio?
5. Há algum caminho menor que eu não listei que remova ≥ 15% sem ser B nem C?

## 10. Revisão 2: o que mudou

| Apontamento (revisão adversarial) | Correção |
| --- | --- |
| C: dois arrays de samplers por TSC não reproduzem `Sampler::MakeKey` (variantes pela imagem) | Samplers por (TSC, variante) com tradução por TIC, ou caminho restrito; portão C-1 |
| C: update-after-bind não preserva a versão de cada draw | Versões imutáveis ou sincronização de reuso, cobrindo o mesmo frame; portão C-2 |
| C0: `CommitResource()` não renova um set específico | Allocator dedicado com identidade, último uso pelo tick de gravação, aposentadoria e sem sobrescrita em uso |
| C: preparar só quando a geração muda não basta (LRU e aliases por draw, conferência da tabela) | Manutenção por draw mantida na conta; conjunto conservador de TICs como problema aberto; ganho de C reduzido |
| B: ganho com tempo de parede e fração misturados | Normalizado: 90/79-90/75 ≈ ×1,14-1,20; ~37-39 ms no pior ponto |
| B + C multiplicados | Não se multiplicam; recalcular estágios depois de C |
| B: leituras além das macros (QMD `ReadBlock`, canal, CPU) | Listadas; harness com leitura depois de escrita entre engines |
| GPU "67% × 1,5" | Só cenário; portão G4 |

