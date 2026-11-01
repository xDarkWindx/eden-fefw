# Passagem de contexto: otimização do eden-fefw (Fortune's Weave)

Documento para continuar o trabalho numa sessão nova. Escrito em 2026-11-01.

## Contexto

- Usuário: xDarkWindx (fala português, tom casual-profissional). Windows, Ryzen 7 5700X, RTX 5060 Ti 16 GB,
  96 GB RAM, monitor 120 Hz. Jogo: **Fire Emblem: Fortune's Weave** (Switch 2).
- Repositórios do usuário (ele autorizou commit e push direto; o `AGENTS.md` foi removido dos dois):
  - `xDarkWindx/eden-fefw`: fork do `Duck912/eden-fefw` (git.eden-emu.dev). Branch de trabalho: **`main`**.
    - `upstream-main`: cópia do main do Duck912 que o usuário sobe (o servidor do Duck912 é bloqueado na
      rede da sessão). Para atualizar: o usuário roda `git fetch upstream` e
      `git push origin upstream/main:refs/heads/upstream-main`; depois a sessão faz o merge no `main`.
    - `upstream-patch`: só as 3 primeiras otimizações sobre o `2dd14bc` do Duck912, com trailer
      `Assisted-by` (regra de IA do projeto Eden), para mandar a ele. Ainda não inclui os commits seguintes.
    - `perf/reuse-bound-pipeline`: branch antigo, já incorporado ao `main`.
  - `xDarkWindx/Eglantine-fook`: fork do Eglantine (branch padrão `nx2`). Só teve o `AGENTS.md` removido.
- Commits: mensagens em inglês, explicando o porquê; terminar com as linhas `Co-Authored-By` e
  `Claude-Session` que a sessão indicar. Autor: `xDarkWindx <darkwindrox@gmail.com>`.

## Fluxo de build e teste do usuário

```powershell
git pull
cmake --build build --target yuzu
Copy-Item build\bin\eden.exe, build\bin\eden.pdb "E:\EmuRoms\Emuladores\eden-fefw-v1.05-windows-x64\" -Force
```

- Ele roda o exe dentro da pasta do v1.05 (lá está a pasta `user`, com chaves, firmware e saves).
- O título da janela só atualiza o hash quando o CMake reconfigura; não serve para saber a versão.
- Build com `-DEDEN_PROFILE_SYMBOLS=ON` já ligado (PDB completo do `video_core`, para perfis).
- Perfis: Visual Studio > Criador de Perfil de Desempenho (Alt+F2) > Processo em Execução > Uso da CPU.
  Cuidados: (1) sem o PDB completo, funções sem símbolo público aparecem com o nome do símbolo vizinho
  (`RuntimeError`, `span<vector>`, `KeplerMemory`...); (2) no código-fonte, o custo de uma chamada aparece
  na linha **seguinte** (endereço de retorno); (3) lambdas com `LAMBDA_FORCEINLINE` somam na linha da chamada.
- Variáveis de ambiente úteis: `EDEN_GPU_PROFILE=1|2` (perfil de GPU; 1 serializa), `EDEN_FINISH_TRACE=1`
  (pilhas dos `Scheduler::Finish` e de quem escreve a tabela de handles), `EDEN_PRESENT_STATS=1`,
  `EDEN_VI2_MAX_RESOLUTION_MODE=1|2|4`.

## Diagnóstico

- A tela emulada é 60 Hz fixa e o jogo usa swap interval 2: **30 FPS só se o frame couber em 33,3 ms**;
  se passar um pouco, cai para 50 ms (FPS em degraus: 30/24/22/20). VSync do PC não influi.
- O gargalo é a **thread da GPU** do emulador (preparação de draws, macros, compute). A GPU real e a CPU
  emulada ficam esperando (o jogo gira lendo `GetCNTPCT`). Resolução e AA quase não mudam o FPS.
- Fortune's Weave usa "arrays de texturas em buffer" (`buffer_source`): todo draw vincula todos os
  registros da tabela, com índices muito repetidos. Era o maior custo por draw.
- Uma vez por frame, a tabela de handles é escrita por um compute shader e baixada com `Finish`
  (`vk_graphics_pipeline.cpp`, `DownloadMemory`). O experimento de pular isso não mudou o FPS e foi revertido.

## O que foi feito (no `main`)

| Commit | O que faz | Teste |
| --- | --- | --- |
| f1c6b38 | Pipeline já resolvido passado ao `UpdateDynamicStates` (evita 3 a 5 buscas por draw) | jogo |
| 9c1e70d | Memo por draw de views (`FillImageViews`) e samplers (`lookup_memo.h`) | A/B: 22 -> 30 FPS no level-up |
| c1fdbfe | Memo de handles de sampler em `PushImageDescriptors`; views escritas no lugar | batalha 26 -> 30 |
| b81abe6 | Decodificador ASTC na GPU sem `Finish` (só `Flush`) | sem teste A/B dedicado |
| fc79ad1 | `UsageTracker`: pula faixas repetidas e limpa só páginas sujas | +2 FPS nos esquadrões; teste aleatório |
| ecf3785 | Interpretador de macros sem cópia do banco de registradores | diferencial com 200 mil programas |
| f30e550 | Samplers do `ConfigureImpl` escritos no lugar | trivial |
| ef68e0d | Opção CMake `EDEN_PROFILE_SYMBOLS` (`/Z7` só no `video_core`) | usado nos perfis |
| 111f225, 439d9f3 | Diagnóstico `EDEN_FINISH_TRACE` (`stack_trace_diagnostics.*`) | usado |
| 4e792b1 | Merge do v1.07 do Duck912 (24ccb4d, 8c6f052: crashes no cache de buffers) | compilado pelo usuário |
| **5735096** | **JIT de macros portado do Eglantine** (`macro_turing_jit.h`), com o DMA de FIFO na semântica do Duck912 | diferencial com 100 mil programas e 1,1 milhão de leituras de DMA; 3,9x no benchmark. **O usuário ainda não compilou nem testou no jogo.** |

Interruptores para A/B: `EDEN_NO_TEXTURE_DEDUPE=1`, `EDEN_ASTC_GPU_FINISH=1`, `EDEN_MME_JIT=0`.
Configuração recomendada ao usuário: ASTC Recompression = Uncompressed. `EDEN_MACRO_UPLOAD_BATCH=1`
(do Duck912) deu +1-2 FPS, mas tem um bug conhecido de tela preta acima de ~56 FPS.

Resultados: level-up, mapa e batalhas em 30 FPS (antes, 18-22). Mapa com 3 esquadrões inimigos: ~24
(antes, ~20). Nessa cena, a thread da GPU fica em ~80% de um núcleo.

## Pendências, em ordem

1. **Testar o build do JIT (5735096)** nos 3 esquadrões, com e sem `EDEN_MME_JIT=0`. Se der erro de
   compilação no MSVC, corrigir (só foi compilado com o GCC, num teste isolado).
2. **Regressão: grama piscando e uma animação de braço estranha.** Apareceu depois do build com o merge
   do Duck912 e das duas mudanças pequenas. Plano: abrir sem `EDEN_MACRO_UPLOAD_BATCH` (as variáveis
   continuam valendo na mesma janela do PowerShell); testar o release oficial v1.07 do Duck912 (pasta nova,
   com junction para a pasta `user`); se o oficial estiver bom, compilar o `f30e550` (antes do merge).
   Interruptores: `EDEN_NO_TEXTURE_DEDUPE=1`, `EDEN_ASTC_GPU_FINISH=1`, `EDEN_MME_JIT=0`.
3. **Próximo ganho grande: dividir a thread da GPU**, como o Eglantine fez (`gpu_split.cpp`,
   `NX2_GPU_THREAD_SPLIT.md` no `Eglantine-fook`; uma thread processa comandos e macros, outra executa os
   métodos; eles mediram 1,4 a 1,6x). É invasivo: fazer atrás de uma variável de ambiente, desligado por
   padrão. Talvez também a memorização de macros (`macro_memo.h` deles).
4. Atualizar o `upstream-patch` com b81abe6, fc79ad1, ecf3785, f30e550 e (depois de testado) o JIT.
5. Release no GitHub para o pessoal do Reddit: zip do `build\bin` com
   `robocopy build\bin <destino> /E /XJ /XD user /XF *.pdb *.ilk *.lib *.exp`. **Nunca** incluir a pasta
   `user` (chaves e firmware). Opcionalmente, GitHub Actions.
6. Ideia arriscada, não iniciada: reaproveitar conjuntos de descritores entre draws. Precisa detectar
   mudanças nas tabelas TIC/TSC e nas imagens.

## Método de teste usado aqui

- Testes diferenciais fora do repositório, compilados com g++ e stubs (`common/logging.h`,
  `common/x64/xbyak.h` -> `<xbyak/xbyak.h>`). O Xbyak foi clonado de `github.com/herumi/xbyak`.
  O interpretador antigo vem do `git show <commit>:src/video_core/macro_turing.h`, renomeado de namespace.
- Arquivos com CRLF: `texture_cache/texture_cache.h`, entre outros. Edite preservando as quebras de linha
  (Python com `newline=''`) e confira o `git diff --stat`.
