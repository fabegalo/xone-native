# Próxima tarefa — M1: ativação de um componente original legível

Atualização de infraestrutura em **2026-11-07**: AppModel agora usa observação `process_exit` por padrão; o handshake precisa de `appmodel_lifecycle owned_fixture`. O runner descartável exige as duas provas. `FramePipe` transporta pixels entre processos desktop próprios até o `Presenter`, com identificação do produtor e falhas limitadas; não importa texturas GPU nem certifica proveniência SystemOS. Próximo check AppModel continua sendo ativação positiva no guest descartável, sem deployment no host. A ligação de um produtor AppModel à ponte e a adaptação de composição original ainda precisam de validação própria. Evidência em [M1-STATUS.md](M1-STATUS.md).

Estado atualizado em **2026-11-07**: o acesso a `xsapi.dll` foi resolvido pelo contexto oficial do pacote Gaming Services, sem alterar ACL/políticas. Com contexto e elevação, o adaptador abriu com `S_OK`; **as montagens somente leitura de `system.xvd` dos OSU1 e OSU2 retornaram `0x80070570`**, sem volume remanescente. A causa da recusa não foi determinada; não repetir elevação/carregamento como se ainda fossem o bloqueio. Diagnóstico reproduzível em `tools/probe_xvd_readonly.ps1`; não é inicializador SystemOS. Os 62 XVDs extraídos e conferidos permanecem em `build-osu/OSU1`, `build-osu/OSU2` e `build-osu/OSU3`, sem misturar perfis; ZIPs originais em Downloads. TLS PE32+ foi validado com fixture própria, sem estabelecer ABI Xbox. A próxima implementação de serviços/WinRT/composição depende de componente original legível e identificado ou de nova evidência que explique a recusa real do provedor; não criar outro loader PE, alterar proteções ou extrair chaves para contornar isso. Evidência em [M1-STATUS.md](M1-STATUS.md).

Base de caminhos atualizada em **2026-11-07**: leitura, inventário e preparação aceitam caminhos longos Unicode sem mover os arquivos. O lançamento próprio com caminho de executável acima de 260 caracteres passou; diretório de trabalho muito longo continua recusado neste host pelo Windows, com diagnóstico explícito. Ao avaliar um componente original, conferir também as operações de arquivo do convidado; a leitura pelo host não certifica esse contrato. Evidência em [M1-STATUS.md](M1-STATUS.md). As pendências de ativação/insumo abaixo permanecem.

## Pendência imediata — concluir a prova AppModel M1.2

Contratos Appx/grafo e backend Windows estão implementados; **a ativação positiva ainda não foi demonstrada**. O host não dispõe de Windows Sandbox/guest descartável e o runner recusou registrar pacotes na instalação do usuário. Falha real de `ActivateApplication` (`0x80073CF1`) e leitura de manifesto não cumprem o segundo modelo de execução. Evidência e comando opt-in em [M1-STATUS.md](M1-STATUS.md).

1. Em guest descartável Windows x64, conta não elevada com deployment de desenvolvimento oficial disponível, executar `tests/appmodel_disposable_test.py` com o bootstrap e `appmodel_component` compilados. Exigir `activation_verified=true`, HRESULT `S_OK`, PID com identidade/caminho conferidos, eventos started/stop/finished, exit 0 e `cleanup_verified=true`. Não registrar no host nem habilitar políticas/instalar certificados para contornar o bloqueio.
2. Validar os ramos positivos e de timeout/encerramento do backend nesse ambiente. Só então declarar um segundo modelo de execução; a fixture é desktop empacotada própria, não prova de UWP ou SystemOS.
3. Pacotes com dependências, classes WinRT/UWP, serviços/capacidades Xbox e origem original continuam bloqueados. Estender somente um contrato observado, incluindo conferência do grafo registrado por identidade/versão/hash antes de autorizar dependências.

O objetivo original abaixo permanece: primeira imagem autêntica da dashboard, com código e recursos originais da mesma revisão. Não repetir inspeção PE/OSU nem interferir na apresentação paralela.

Estado em **2026-11-05**: M1.1 entrega inspeção de imports normais/atrasados, exports/forwarders, API Sets e recursos de ativação disponíveis, integrada ao JSON determinístico de `xboxone-systemos prepare`. A identidade e a revisão de cada provedor precisam ser verificadas; presença de export não autoriza sua execução. Oito grupos afetados passaram no Windows; CI aprovada em Windows (9/9), Linux (7/7) e macOS (7/7), run [41960184838](https://github.com/fabegalo/xboxone-native/actions/runs/41960184838), commit `4b336b5`, incluindo a cobertura obrigatória de symlinks. Evidência e limites em [M1-STATUS.md](M1-STATUS.md).

Os arquivos do OSU2 continuam expondo somente contêineres XVD, sem componente original diretamente legível. Nenhum boot, ativação original ou frame autêntico demonstrado. O próximo trabalho depende desse insumo; não repetir infraestrutura sintética aprovada nem pesquisas de criptografia já concluídas.

## Objetivo concreto

Identificar **um componente original do Xbox One já legível por acesso autorizado**, com módulos/recursos da própria revisão, preencher o manifesto implementado e estender o menor contrato realmente bloqueante até uma tentativa original verificável. O shell e a primeira imagem continuam dependentes desse insumo; não repetir a infraestrutura sintética já aprovada.

## Entradas e seleção

- Preservar os três ZIPs em Downloads. Os perfis da pré-checagem referenciam seus membros sem extração, duplicação ou mistura.
- O SystemOS de OSU2 e OSU3 é idêntico por SHA-256. Para a primeira análise histórica, uma única referência OSU2 evita trabalho duplicado sobre esse volume, mantendo OSU3 como pacote distinto pelo updater. Isso é prioridade de investigação, não uma versão de dashboard escolhida ou comprovadamente mais fácil.
- O OSU1 é uma alternativa posterior válida para investigação do One; `ScarlettHost.xvd` permanece fora do alvo One. Não transferir seus recursos ou versões para o conjunto histórico.
- Falta obter uma entrada original **já legível por um acesso autorizado**. Reconhecer cabeçalhos de OSU não supre isso. Não implementar bypass, extrair chaves ou afirmar que um flag de cabeçalho tornou o conteúdo acessível.

## Trabalho quando a entrada estiver disponível

1. Executar `inventory <raiz-do-conjunto> --json` e preencher o manifesto v1 de [M1-STATUS.md](M1-STATUS.md): revisão exata, identidade de fonte, origem declarada, caminho/hash/procedência por arquivo e bindings explicitamente auditados. Referenciar os originais no lugar, sem cópias; o manifesto não autentica a revisão declarada.
2. Identificar o shell e seu modelo de pacote/ativação a partir dos arquivos efetivamente encontrados, sem presumir nomes de executável ou a geração visual da dashboard.
3. Aplicar o grafo PE já implementado ao conjunto legível. Auditar o mapeamento exato de API Sets da revisão e interpretar seus metadados reais de pacote/WinRT/SxS. O parser preserva RT_MANIFEST/RT_VERSION e marca CLR/TLS; não resolve classes WinRT, schemas API Set, manifestos externos, serviços, assets ou comunicação com HostOS automaticamente.
4. Usar `xboxone-systemos prepare <manifesto>` para registrar dependências ausentes ou não implementadas. O backend atual aceita só EXE Win32 console com kernel32 auditado; imports privados/atrasados, WinRT, serviços, kernel NT e dispositivos exigem trabalho específico. Não escolher outro arquivo/OSU por basename.
5. Estender somente o contrato observado, com teste próprio. Quando kernel/dispositivo for indispensável, registrar símbolo/serviço/IOCTL e avaliar backend isolado. Só executar `run` após auditoria de ativação e dependências; saída 0 sozinha não prova inicialização original.
6. Conectar a superfície gráfica produzida pelo componente original de SystemOS ao `xboxone::presentation::Presenter` através de `SystemOsSurfaceHandoffDesc` / `ExternalFrameView` com `FrameOrigin::systemos_component`, informando o SHA-256 verificado do executável e a revisão `system_version_raw` do perfil isolado correspondente.

## Caminho técnico para conectar a saída gráfica real do SystemOS

1. **Identidade e isolamento prévios:** o perfil de SystemOS precisa validar o componente original legível e seus recursos da mesma revisão, preenchendo `FrameProvenance` (`origin = FrameOrigin::systemos_component`, `component_id`, `component_sha256` de 64 caracteres hexadecimais e `system_version_raw != 0`).
2. **Interceptação/ponte de composição do SystemOS:** quando o contrato real de composição/swapchain do shell original (`dcomp` / `dxgi` / `d3d11.x` / `d3d12` do SystemOS) for mapeado a partir dos imports reais do binário legível, o adaptador de composição capturará a superfície final produzida pelo componente original.
3. **Entrega ao `Presenter`:**
   - **Estágio inicial (staging verificado):** mapear a superfície produzida pelo componente original para `ExternalFrameView` (`bgra8_unorm` ou `rgba8_unorm`, respeitando `row_pitch_bytes`) e submeter a `Presenter::present_frame`.
   - **Estágio direto na GPU:** usar o contrato `SystemOsSurfaceHandoffDesc` (`SurfaceHandleKind::d3d12_resource` ou `dxgi_shared_handle`) para copiar/apresentar diretamente a textura na fila D3D12 sem cópia intermediária pela CPU.
4. **Critério de primeira imagem autêntica:** somente quando `stats().authentic_systemos_frames > 0` for produzido por execução real do componente original autorizado, com log, hash verificável e captura correspondente. Testes sintéticos (`synthetic_frames`) permanecem estritamente separados.

## Critérios de conclusão

- Um conjunto legível coerente por versão, com componente de shell identificado e hashes/procedência registrados.
- Grafo de dependências com falhas explícitas e nenhuma substituição silenciosa entre OSUs.
- Decisão justificada para a primeira tentativa de ativação, com testes focados sobre o contrato alterado.
- Atualizar `STATUS.md`, `docs/M1-STATUS.md` e este arquivo. Primeira imagem exige observar código e recursos originais renderizando, com identidade, logs e captura; rótulos de proveniência e contadores gráficos não bastam.

Se a entrada legível continuar ausente, manter o bloqueio identificado em vez de repetir hashes, reexecutar suítes aprovadas, gerar cópias de firmware ou simular progresso de boot.
