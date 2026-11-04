# Variáveis de ambiente que restaram

Desde 2026-11-03 o fork não segue mais upstream, e o código de diagnóstico da fase de bring-up foi removido
(capturas `EDEN_NATIVE_*`, traces, profilers, experimentos A/B). Ficaram só estas:

| Variável | Efeito |
| --- | --- |
| `EDEN_NO_HEAP_REUSE=1` | Desliga o reaproveitamento de bancos de texturas e dos descritores (para isolar textura errada). |
| `EDEN_TEXTURE_TABLE_STATS=1` | Loga a cada 4 s as estatísticas das tabelas de texturas e dos bancos. |
| `EDEN_BACK_THREAD_STATS=1/2` | Loga a cada 4 s onde a thread que prepara os draws gasta o tempo (trechos do `PrepareDraw`, descritores, esperas no scheduler, worker do Vulkan, locks dos caches, frente do split). `2` serializa os carimbos (`lfence`), para conferir um número decisivo. Custa ~1-2%: medir draws/s com ele desligado. Ver `docs/back-thread-split-plan.md`, fase 0. |
| `EDEN_GPU_SPLIT=0/1/2` | GPU emulada em uma thread (0), métodos na thread de trás (1) ou também macros na da frente (2, padrão). |
| `EDEN_MME_JIT=0` | Macros nativas no interpretador em vez do código compilado. |
| `EDEN_NATIVE_FRONTEND=lower/ampere/auto/compare` | Frontend de shaders nativos (padrão `lower`). |
| `EDEN_WINDOW_TITLE` | Título da janela no eden-cli (usado internamente pelo próprio frontend). |

Os testes em `src/tests/video_core/native_shader.cpp` ainda leem `EDEN_NATIVE_SHADER_FIXTURES` e afins (opcionais,
só para rodar os testes com fixtures).
