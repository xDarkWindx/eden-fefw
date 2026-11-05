# Preparação para três otimizações futuras (sem obrigação na implementação atual)

Escrito em 2026-11-05 sobre `60a12ae`, a partir de uma recomendação do revisor. Graus de certeza como nos
outros planos: **verificado** (lido no código, com `arquivo:linha`), **medido** (número de um doc ou commit,
dizendo de quem), **hipótese**.

## 0. Propósito e limites

Três otimizações de desempenho do FE ficaram de fora até agora porque mudam demais a estrutura: separar a
engine do rasterizador, reaproveitar descriptor sets por banco e um heap bindless persistente. Este documento
**não** as coloca na implementação atual. Ele faz duas coisas:

1. Diz o que cada uma vai exigir dos contratos de `docs/architecture-plan.md` (seção 2.2), para que os contratos
   novos não fiquem específicos demais para command lists.
2. Dá a cada uma requisitos, um cenário de teste de aceitação e os custos a medir, para que ela possa entrar
   depois **localmente, atrás de chave, com testes e medição próprios, sem redefinir as garantias centrais de
   memória e sincronização** (critério de sucesso desta preparação).

Isso prepara o caminho; não pressupõe ganho nem promete implementação trivial.

**Regra:** nada de cópias, atômicos ou alocações por draw só por causa de uma possibilidade futura. A preparação
é de vocabulário e de interface, não de código no caminho quente.

## 1. Fatos compartilhados (e por que não são uma "versão" só)

As três otimizações e os contratos atuais precisam dos mesmos tipos de fato. Eles se relacionam, mas são fatos
diferentes, e misturá-los num único número de "versão" esconde exatamente os erros que o T1 achou.

| Fato | O que responde | Quem é o dono | Exemplo de erro se confundido |
| --- | --- | --- | --- |
| **Versão de bytes** | Que produtores estão incluídos nestes bytes | Caches (thread de trás) e as transferências | Usar bytes de antes do produtor (T1: `no_dependency`) |
| **Geração de descritor** | Que objeto Vulkan (view, sampler) um índice ou slot representa | Cache de texturas | Reaproveitar uma view apagada (f92cd68 deu geração própria ao reuso de descritores) |
| **Identidade de mapeamento** | A que memória física um endereço da GPU aponta | Memory manager | Transferência válida para o mapeamento antigo (T1: `remap`) |
| **Tempo de vida na GPU** | Até quando a GPU pode ler um recurso já enviado | Scheduler (ticks e fences) | Reciclar um set ou slot ainda em uso por um draw em voo |
| **Dependência satisfeita** | A condição que o jogo pediu vale | Sincronização (frente + quem satisfaz) | Começar antes de o produtor de outro canal existir (T1: `other_channel_missing_dependency`) |
| **Trabalho processado** | A thread de trás já gravou isso no fluxo | Ring do split | Tomar a posição no ring por conclusão |
| **Conclusão física** | A GPU terminou | Fence própria da operação | Tomar o syncpoint em Fast por conclusão (T1: `syncpoint_trusted_as_ready`) |
| **Publicação** | Outra thread pode ler o resultado | Quem produziu o resultado (release/acquire) | Ler a transferência antes da fence e da publicação |
| **Invalidação com motivo** | Por que algo deixou de valer | Quem detecta a mudança | Invalidar tudo por qualquer motivo (custo) ou nada (erro) |
| **Preservação e aposentadoria** | Uma versão antiga ainda tem consumidor em voo | Dono do recurso + tempo de vida | Sobrescrever o que um draw em voo ainda lê |

Os três últimos aparecem em todas as otimizações abaixo: é aí que a preparação comum rende.

## 2. Separação entre engine e rasterizador

**Contexto (verificado nos docs daqui):** é a opção B-split de `docs/back-thread-split-plan.md`, seção 9. A
thread de trás fica com os métodos da engine e o `DrawManager`, e uma thread nova com o `RasterizerVulkan`. Foi
adiada porque registradores não bastam: a engine grava memória do guest entre draws (`ProcessCBMultiData` →
`WriteBlockCached`), o rasterizador lê essa memória no draw, e as flags de sujeira são consumidas por vários
leitores (seção 3.14 e 9 daquele plano).

**O contrato precisa definir:**
- Que estado de registradores pertence a cada operação (o pacote de um draw).
- Que dados de memória são copiados, preservados ou consultados na posição certa do fluxo: constant buffers,
  argumentos indiretos, tabelas de handles. É o contrato de memória com "posição do consumidor" = a posição do
  draw, não a de uma entrada de lista.
- Quem administra caches, recursos e scheduler (hoje, a thread de trás; depois, a do rasterizador).
- Como representar dependências, resultados e conclusão sem um drain global por operação (semáforos, leituras
  de memória por macros, `AccelerateDMA` com retorno).

**Primeiro passo possível, na mesma thread:** a engine produz um pacote de draw e o rasterizador o consome
logo depois, sem thread nova. Isso testa a captura (registradores e versões de memória) sem mudar o tempo, e
um modo de verificação compara o resultado com o caminho direto.

**Teste de aceitação:** enfileirar o draw A; modificar registradores e memória (constant buffer, argumento
indireto) para o draw B; atrasar o rasterizador; verificar que A usa os dados da sua posição e B os seus.

**Custos a medir no FE:** tamanho e custo de montar os pacotes, tráfego de memória das cópias, esperas novas
(cada consulta de volta ao rasterizador vira espera). Só depois de medidos dá para dizer se outro split
compensa (o plano do split estimou um teto de ×1,5, **não sustentado**, seção 3.14).

## 3. Descriptor sets persistentes por banco

**Contexto (verificado nos docs daqui):** o reuso de bancos já existe (`texture_cache/texture_bank_reuse.h`,
`docs/handoff-aggressive.md`, "Estado"). O banco resolvido (views e samplers) é reaproveitado entre draws
quando a tabela, o snapshot e a `bank_generation` batem. Os descritores escritos são copiados em bloco
(`AddEntries`). Mas o `VkDescriptorSet` ainda é atualizado a cada draw. Na RTX do usuário o caminho é push ou
set, não descriptor buffer (**medido**, `docs/back-thread-split-plan.md` 3.14). Reaproveitar o próprio set
pularia a atualização e a alocação.

**O contrato precisa distinguir:**
- Identidade do banco, geração dos descritores e compatibilidade do layout exigido pelo pipeline.
- Mudança no conteúdo da imagem (os bytes; o set continua valendo) **versus** mudança no descritor ou no objeto
  Vulkan (o set deixa de valer).
- Validade lógica do set **versus** o tempo de vida dele enquanto a GPU executa.
- Reutilização do set **versus** preparação obrigatória das imagens e resolução de aliases (`PrepareImage`,
  `SynchronizeAliases`), que continua por draw.

**Alocação:** o allocator precisa preservar **aquele set específico** até o último uso terminar. Pegar outro
set livre do pool, ou só atualizar o tick de uma alocação nova, não garante isso.

**Teste de aceitação:** reutilizar o banco entre pipelines com layouts compatíveis; alterar o banco enquanto um
draw antigo continua em voo; verificar que o draw antigo conserva os descritores anteriores, e que pipelines com
layout incompatível não reutilizam o set.

**Custos a medir:** sets retidos (memória do pool), atualizações evitadas por segundo e frequência com que
draws seguidos com o mesmo banco usam layouts compatíveis. Esse último é a condição para valer a pena: sem um
contador dele, o protótipo não começa.

## 4. Heap bindless persistente

**Contexto:** é a opção C dos docs daqui (~10-20% estimados na thread de trás, semanas no recompilador;
`docs/back-thread-split-plan.md` 3.14, **estimativa**, não medida).

**O desenho precisa permitir:**
- Identidade estável dos recursos e gerações de descritor por slot.
- Preservar descritores antigos enquanto houver consumidores em voo; aposentar um slot só pela conclusão real
  na GPU do último uso, não pela posição no ring nem por um syncpoint.
- Trocar imagens e samplers sem alterar retroativamente draws em voo.
- Tratar destruição, recriação, mudança de resolução (rescaling) e aliases.
- Evoluir a representação dos índices usada pelos shaders (recompilador), sem fixar o formato agora.
- **Identidade do sampler inclui a interpretação efetiva:** o TSC bruto do guest não determina sozinho o
  descritor final (comparação de profundidade pedida pelo shader, filtro e anisotropia ajustados pelo emulador,
  reescala). A chave do slot é a do descritor final.

**Teste de aceitação:** substituir um recurso com consumidores antigos em voo; verificar que cada consumidor
acessa a sua versão e que nenhum slot é reciclado antes da hora.

**Custos a medir:** limites de descritores do dispositivo (desktop primeiro; Android fica no caminho atual),
memória do heap, atualizações por evento contra as por draw de hoje.

## 5. O que conferir nas interfaces atuais (passo 3 da sequência)

Depois do diagnóstico e da correção dos contratos de coerência, conferir se as interfaces do
`tools/gpu-contract-harness/contracts.h` (e o que for para `src/video_core/` no T3) suportam os três cenários
sem acesso indevido aos caches nem dependências implícitas:

| Pergunta | Por que importa |
| --- | --- |
| `Transfer` aceita "posição do consumidor" de qualquer tipo (entrada de lista, draw, dispatch), ou supõe uma entrada de lista? | Separação engine/rasterizador |
| A validade (`valid_until`) é só de bytes e mapeamento, separada de geração de descritor? | Descriptor sets e heap (fatos diferentes, seção 1) |
| Existe um conceito de "último uso na GPU" (tick/fence) que um recurso retido possa consultar, separado da conclusão de uma transferência? | Sets persistentes e slots do heap |
| Uma `Dependency` pode ter como resultado um valor publicado (não só "satisfeita"), para consultas que hoje voltam do rasterizador na hora? | Separação engine/rasterizador |
| Invalidação carrega o motivo? | Medir por que reusos falham (como a linha "Texture banks" já faz) |

Se alguma resposta for "não", a mudança de interface é decidida nesse passo, explicitamente, e não dentro da
implementação de uma otimização.

## 6. Declaração obrigatória de cada extensão

Como no plano de arquitetura (seção 2.2): novas esperas, recursos retidos, custo quando não utilizada,
caminhos afetados e o teste correspondente. Cada uma atrás de chave própria, com A/B no FE.

## 7. Sequência recomendada (do revisor)

1. Acrescentar estes requisitos e cenários ao plano (este documento).
2. Concluir o diagnóstico e a correção dos contratos de coerência atuais (T3 e T4).
3. Conferir se as interfaces suportam os três cenários (seção 5).
4. Escolher o primeiro protótipo pelos contadores. Os descriptor sets persistentes são um candidato,
   condicionados à reutilização entre layouts compatíveis (seção 3).
5. Avaliar a separação engine/rasterizador e o heap bindless em experimentos independentes.

## 8. Perguntas abertas

1. Quantos draws seguidos com o mesmo banco usam layouts compatíveis no FE? (Decide se a seção 3 vale.)
2. Quanto da thread de trás é hoje atualização de descriptor sets (no worker do Vulkan ou na própria thread)?
   O perfil da praça mostrou `PushImageDescriptors` em ~8,5% (**medido**, `docs/handoff-aggressive.md`), que não
   é exatamente isso.
3. Para a separação engine/rasterizador: quais leituras do rasterizador de volta para a engine acontecem por
   draw, e quantas por segundo no FE? Cada uma vira espera.
