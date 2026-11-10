# M0 — estado e evidências

Atualizado em **2026-11-04**, durante a retomada com OSU1/OSU2/OSU3. Estado máximo: identificação de contêineres, seleção por alvo e pré-checagem. Nenhum executável original de SystemOS foi carregado ou iniciado; não há primeira imagem da dashboard.

## Separação dos OSUs para limpeza

| Insumo fornecido | Classificação para este projeto | Decisão |
|---|---|---|
| `OSU1.zip` — 6.258.043.961 bytes | Atualização com candidatos Xbox One e ramo Scarlett/Series | **Manter**. Contém `system.xvd`, `host.xvd`, apps e recursos úteis ao One; não é descartável inteiro por conter Scarlett |
| `OSU2.zip` — 2.000.063.961 bytes | Recuperação histórica do Xbox One original, componentes de 2015 | **Manter** como conjunto independente |
| `OSU3.zip` — 2.000.063.824 bytes | Recuperação histórica do Xbox One original, componentes de 2015 | **Manter**. Sete membros iguais ao OSU2, mas `updater.xvd` diferente |
| ZIP exclusivamente fora do Xbox One | **Nenhum identificado entre os três** | **0 remoções** |

Atribuir a família de um OSU não equivale a autenticar sua assinatura ou provar execução no PC. O programa usa o rótulo OSU do catálogo anterior, vinculado ao hash integral do ZIP, e exige o layout esperado. Um arquivo desconhecido permanece sem classificação e não vira candidato automático à exclusão.

Dentro do OSU1, `ScarlettHost.xvd` é a alternativa de HostOS que não entra na seleção Xbox One. `nvmefw.xvd` e outros atualizadores de hardware não entram na ativação de shell; a exclusão dessa etapa não os transforma em arquivos seguramente descartáveis. `uERA.xvd` pertence à pesquisa de GameOS, e `systemtools.xvd` é referência de ferramentas de desenvolvimento. Os ZIPs permanecem intactos: excluir de um perfil não significa apagar conteúdo ou criar uma segunda cópia do pacote.

Referências públicas: a pesquisa [Firmware do Xbox One](https://xboxoneresearch.github.io/wiki/operating-system/firmware/) aponta o OSU1 para a linha One; a [documentação de camadas e volumes](https://xboxoneresearch.github.io/wiki/operating-system/xbox-operating-system/) distingue SystemOS, HostOS e ERA. O portal [Xbox Offline System Update](https://support.xbox.com/en-US/help/hardware-network/console/offline-system-update) foi consultado, mas sua página dependente de JavaScript não forneceu texto legível nesta sessão. A classificação dos arquivos locais combina o estudo anterior, seus cabeçalhos, datas e layouts; não foi inferida somente de x86-64 nem de extensão `.xvd`.

## Identidades verificadas nesta retomada

| Pacote | SHA-256 do ZIP, recalculado | Revisão bruta do SystemOS |
|---|---|---|
| OSU1 | `b79bd2ae481b65029ed5d7d8056a0ed57a5fcaebaf02ba6e074e4bd3e56b29aa` | `0x800a000065f425af` |
| OSU2 | `74ca6bb17197689e9861a105021a8aaaf827b7a3b4b13e35a5562de32a04b326` | `0x341400004723ad20` |
| OSU3 | `07092f55b25ff8615da866bfcbf7d162c9ed4f9810b1eb80ce336f9e560cac07` | `0x341400004723ad20` |

SHA-256 de `system.xvd`: OSU1 `91bd946ace47245c9c6856614e8c7b471aa587807aeacde93e75efb0319b65ae`; OSU2/3 `52d048663a33d30b2a46a6817b496c7d878e8bb4f3af36a1a15c20acb0bd3dcc`. Estes hashes dos membros provêm do estudo completo anterior, não de uma nova leitura integral dos membros. A rodada atual recalculou o hash dos três ZIPs, reinterpretou os prefixos com o parser C++ e exigiu igualdade dos nomes, tamanhos, CRC declarado, prefixos e metadados com a referência.

O catálogo usado foi `docs/research/osu-packages.metadata.json`, preservado sem alterações. O programa não normaliza os campos de versão brutos como se fossem uma versão comprovada da dashboard. A identidade do perfil é o hash do ZIP; os dois OSUs históricos continuam com perfis distintos apesar de compartilharem SystemOS.

## Avanço de implementação

`tools/systemos_preflight.py` implementa a próxima checagem do fluxo de conteúdo:

1. Calcular SHA-256 do ZIP e encontrar exatamente uma referência correspondente, independentemente do nome atual do arquivo.
2. Ler os prefixos sem extração com `study_update_zip.py`, que chama o parser nativo de `xone-native`.
3. Recusar referência incompleta, membro sem SHA-256 completo, mudança de arquivo, conjunto de membros divergente ou metadados adulterados.
4. Criar **um perfil por pacote**, com referências aos membros do próprio ZIP; não buscar módulos ou recursos em outra revisão.
5. Separar volumes SystemOS, referências de HostOS/templates e componentes fora desta etapa. Detectar divergência de revisão entre os candidatos SystemOS.
6. Reportar bloqueios para a primeira imagem original sem conceder um estado de boot por uma inspeção bem-sucedida.

Resultado real:

| Pacote | Candidatos SystemOS/recursos | Referências HostOS/templates | Fora da seleção SystemOS |
|---|---:|---:|---:|
| OSU1 | 37 | 2 | 7 |
| OSU2 | 2 | 4 | 2 |
| OSU3 | 2 | 4 | 2 |

Os dois candidatos dos OSUs históricos são `system.xvd` e `systemaux.xvd`. No OSU1 incluem também `systemauxf.xvd`, `systemmisc.xvd`, `all-langs.xvd` e idiomas. Não houve divergência de revisão entre os candidatos selecionados em cada pacote. A revisão distinta do `uERA.xvd` não é tratada como erro de SystemOS, pois é outro ambiente e permanece fora desse conjunto.

As sugestões de pontos de montagem (`C:` para system, `X:`/`Y:` para apps e `M:` para misc) vêm da pesquisa pública. São metadados de planejamento: não houve montagem experimental. Recursos de idioma não recebem uma letra de volume inventada. O programa não executa firmware, não descriptografa payloads e não reempacota os originais.

## Bloqueios observados para a dashboard

- Os **62 XVDs** têm criptografia habilitada segundo o cabeçalho; nenhum apresenta o bit `EncryptionDisabled`. Isso não é uma validação criptográfica independente, mas impede assumir payload legível.
- Nenhum executável PE interno, manifesto de app, shell ou recurso correspondente foi disponibilizado em forma legível pelos arquivos examinados. Ainda não existe um grafo real de imports/ativação da dashboard.
- Não há backend de ativação de SystemOS implementado. Não é possível escolher corretamente entre execução direta e isolamento/VM antes de analisar as dependências do componente original.
- Os ZIPs de atualização não incluem os volumes inicializados `settings-saved.xvd` e `user.xvd`. Templates não equivalem automaticamente a um disco de console pronto. Não foram criados arquivos vazios ou presumidos serviços de persistência funcionais.

O retorno real do programa é **1**, com `diagnostic_completed: true` e `can_boot_original_dashboard: false`. Código 2 indica entrada inválida ou divergente. Desativar o flag de criptografia em uma fixture também não concede boot: shell, grafo de dependências e ativação continuam pendentes.

## Validação focada e reprodução

Ambiente: Windows x64, Python do runtime local e inspetor C++20 já compilado com MSVC 19.44. Não houve alteração C++ nesta retomada; o CMake foi reconfigurado para registrar somente o teste novo. As suítes anteriores e sessões gráficas não foram repetidas.

```powershell
python tests/systemos_preflight_tests.py build-checked/xone-native.exe
```

**9 testes aprovados** com conteúdo sintético: identidade por SHA-256 após renomeação, ZIP alterado, metadados de referência adulterados, referência incompleta/sem hash, catálogo malformado sem traceback, versão misturada, separação Scarlett/ERA, flag sem criptografia sem falso boot e CLI bloqueada sem candidatos à exclusão. Pacotes desconhecidos são indeterminados, não declarados ausentes do alvo One. O teste está disponível no CTest como `systemos_preflight_regressions`; `ctest --test-dir build-checked -N -R '^systemos_preflight_regressions$'` confirmou o registro.

Aplicação aos arquivos reais:

```powershell
python tools/systemos_preflight.py 'C:\Users\fabeg\Downloads\OSU1.zip' 'C:\Users\fabeg\Downloads\OSU2.zip' 'C:\Users\fabeg\Downloads\OSU3.zip' --reference docs/research/osu-packages.metadata.json --inspector build-checked/xone-native.exe > build-checked/m0-systemos-preflight.json
```

O JSON de diagnóstico está em saída de build ignorada, sem nova cópia de evidência em documentos estáveis. Nenhum arquivo original foi removido, movido, extraído ou executado. Próximo avanço delimitado em [NEXT-TASK.md](NEXT-TASK.md).

## Encerramento da infraestrutura M0 solicitada

Inventário, estudo OSU1/2/3 e pré-checagem foram publicados diretamente na `main` em `7307692`, com originais e hashes preservados. O avanço executável posterior e seus bloqueios pertencem a [M1-STATUS.md](M1-STATUS.md); isso não converte a identificação M0 em boot original.

## Backend de apresentação Windows x64 (`xboxone_presentation`)

Implementado para preparar o caminho de exibição sem depender do carregador de SystemOS e sem simular a dashboard:

- **Arquivos:** `include/xboxone_native/presentation.h`, `src/presentation_backend.h`, `src/presentation.cpp`, `src/presentation_d3d12.cpp` e `tests/presentation_tests.cpp`.
- **Execução real D3D12 + Win32:** janela `HWND` com ciclo de vida explícito (`initialize`, `pump_messages`, `resize`, `present_frame`, `wait_for_gpu`, `recover_swapchain`, `close_window`, `shutdown`), swapchain flip-model (`IDXGISwapChain3`), alocadores de comando por backbuffer, sincronização CPU/GPU via `ID3D12Fence` e buffer de upload persistente alinhado a `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` (256 bytes).
- **Prevenção de falso boot:** quadros de teste usam `FrameOrigin::synthetic_test`, rotulam a janela como `[SYNTHETIC TEST - NOT SYSTEMOS]` e nunca incrementam `authentic_systemos_frames`. Quadros ou superfícies `FrameOrigin::systemos_component` são rejeitados se não trouxerem `component_id`, SHA-256 de 64 dígitos hexadecimais e `system_version_raw` não nulo.
- **Interfaces futuras:** `BackendKind::vulkan` e `BackendKind::metal` compartilham o mesmo contrato C++20 e falham com diagnóstico explícito enquanto seus backends nativos não forem implementados.
- **Validação focada:** compilado apenas `xboxone_presentation_tests` (`MSVC 19.44.35207` x64) e executado `ctest --test-dir build-checked -R "^presentation_regressions$" --output-on-failure -V`, aprovando **109 verificações** em `NVIDIA GeForce RTX 4070` (`software=false`) e no adaptador `WARP` (`software=true`), com readback da backbuffer D3D12 comprovando integridade pixel a pixel.
