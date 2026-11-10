# Estudo dos arquivos fornecidos

Data da análise: **2026-11-04**. O projeto continua no M0: inspeção de conteúdo, sem execução de SystemOS, dashboard ou jogos. Os arquivos fornecidos em Downloads permaneceram no local original. Nenhum membro dos ZIPs foi extraído para disco ou executado. Os JSONs desta pasta contêm apenas metadados e hashes.

## Relação com o x360-native

A referência local foi `Desktop/x360`, especialmente `docs/architecture.md` e `docs/guides/content-layout.md`. A direção compartilhada é executar componentes originais, identificar conteúdo por SHA-256, isolar versões e validar dashboard → jogo → dashboard com evidência de cada etapa. Conteúdo, frontend, serviços de compatibilidade e apresentação têm fronteiras distintas.

O x360 prepara PowerPC por recompilação estática. Para Xbox One, pretende-se aproveitar x86-64 diretamente quando demonstrado, resolvendo serviços e fronteiras HostOS/SystemOS/GameOS. ReXGlue, XAM e o tradutor Xenos não foram adicionados como dependências. Compilar o inspetor não comprova boot ou renderização de componentes Microsoft.

## Azimuth

Identidade do ZIP: 6.835.590 bytes, SHA-256 `f0e603a4548ae7c40c1cef584ce691dbeae0bc178666bed896ab87ab6cd52994`.

O pacote contém **23 arquivos**, totalizando 27.489.180 bytes descompactados: 11 imagens PE AMD64 e 12 arquivos de recursos/licenças. O executável principal tem 8.613.376 bytes, subsistema gráfico e seis seções. Sua tabela estática de imports contém 14 bibliotecas, incluindo `d3d12.dll`, `dxgi.dll`, contratos WinRT, `ole32.dll`, `DINPUT8.dll`, `DWrite.dll` e APIs Win32. Há diretório TLS no executável e nas duas DLLs `azimuth-tls-*`; isso não demonstra como o carregador trata o TLS dos jogos.

As DLLs FFmpeg e Visual C++ são distribuídas separadamente. `azimuth.pipelines` ocupa 11.389.290 bytes; seu formato e uso exato não foram determinados. Não há DLL separada chamada `kernelx.dll` ou `d3d11_x.dll` no ZIP. Nomes embutidos no executável não estabelecem sua implementação nem que uma função tenha sido exercitada.

A [página oficial do Azimuth](https://almost-there-games.itch.io/azimuth) descreve tradução de WinRT Xbox para Win32 e D3D11.X para D3D12. Isso orienta a pesquisa da camada de jogos; este estudo não comprova boot da dashboard original. O ZIP não inclui fontes do Azimuth. `data/licenses.txt` reserva seus arquivos próprios e identifica terceiros. Nenhum arquivo desse pacote foi incorporado ao runtime.

Implicação: investigar carga PE, imports, relocação, TLS, exceções/unwind, ativação WinRT e apresentação como contratos distintos. O executável x64 não resolve a inicialização de SystemOS. Evidência por arquivo: [azimuth-windows.metadata.json](azimuth-windows.metadata.json).

## Pacotes OSU

Os três ZIPs têm estrutura `$SystemUpdate/`; todos os **62 membros de arquivo** possuem `msft-xvd` no offset `0x200`. Isso identifica formato, sem autenticação Microsoft. Foram calculados hashes completos dos ZIPs e dos XVDs por streaming, verificando também o CRC do ZIP. Os prefixos foram interpretados pelo parser C++ do projeto via `tools/study_update_zip.py`.

| Pacote | Tamanho do ZIP | XVDs | Evidência local relevante |
|---|---:|---:|---|
| OSU1 | 6.258.043.961 bytes | 46 | `system`, auxiliares, ferramentas, idiomas, `host`, `ScarlettHost`, firmware e `uERA` |
| OSU2 | 2.000.063.961 bytes | 8 | `system`, `systemaux`, `host`, `sosinit`, `sostmpl`, template, ferramentas e updater |
| OSU3 | 2.000.063.824 bytes | 8 | Mesmos nomes do OSU2; SHA-256 diferente somente para `updater.xvd` |

**OSU2 e OSU3 têm sete membros idênticos por SHA-256**, incluindo `system.xvd`. Os dois `updater.xvd` têm o mesmo tamanho e metadados principais, mas hashes diferentes; não são intercambiáveis pelo nome. O `system.xvd` histórico declara criação FILETIME em 2015-09-04 UTC; o do OSU1 em 2026-10-22 UTC. Datas declaradas não autenticam origem nem identificam sozinhas uma versão de dashboard.

O campo de conteúdo de `system.xvd` é `2` e o de `host.xvd` é `7` nos três pacotes. A [referência dos tipos XVD do pesquisador original](https://github.com/emoose/xvdtool/blob/master/LibXboxOne/XVD/XVDEnums.cs) identifica esses valores como SystemOS e HostOS. `ScarlettHost.xvd` e `nvmefw.xvd` no OSU1 indicam conteúdo a separar por hardware; não se deve assumir que todo esse pacote é exclusivo do Xbox One. A atribuição exata por revisão ainda exige evidência adicional. OSU2/OSU3, com componentes datados de 2015 e volumes SystemOS/HostOS, são candidatos históricos para Xbox One, não uma base de Xbox 360.

| Componente | Papel para a pesquisa | Regra de seleção |
|---|---|---|
| `system.xvd` | Candidato principal de SystemOS | Selecionar um pacote e preservar seu conjunto |
| `systemaux.xvd`, `systemauxf.xvd` | Auxiliares a investigar | Não preencher ausências com outra revisão |
| `host.xvd` | Dependências de HostOS/dispositivos | Não assumir execução como aplicação Win32 |
| `ScarlettHost.xvd`, `nvmefw.xvd` | Conteúdo separado até identificação por hardware | Não selecionar como fallback do Xbox One |
| `uERA.xvd` | Ambiente ERA, inferido do nome | Confirmar metadados e binários antes de uso |
| Idiomas, templates e ferramentas | Recursos e infraestrutura da revisão | Preservar identidade e procedência |
| `updater.xvd` | Atualizador offline | Não é o executável da dashboard |

Os flags dos `system.xvd` são `1` no OSU1 e `9` no OSU2/OSU3; o bit público de desativação de criptografia não está marcado. Não foi obtido acesso aos executáveis internos. Versões de pacote e de sistema requerido foram preservadas como campos brutos, sem conversão automática em versão de dashboard. O [mapeamento público dos campos de cabeçalho](https://github.com/emoose/xvdtool/blob/master/LibXboxOne/XVD/XVDStructs.cs) foi consultado como referência de formato; não foi incorporado código GPL, decriptação ou material de chave.

Manifesto: [osu-packages.metadata.json](osu-packages.metadata.json). Os três pacotes permanecem independentes. Hashes de prefixo identificam apenas os primeiros `0xE00` bytes; o SHA-256 do membro identifica o XVD completo. Nenhum substitui assinatura Microsoft ou validação da árvore de integridade interna.

## Implementação e reprodução

O inventário calcula SHA-256, agrupa imports e registra candidatos locais sem certificar compatibilidade. O adaptador ZIP usa a mesma biblioteca C++ para leitura limitada de prefixos, sem extração. Os arquivos recebidos expuseram um erro real: o parser procurava `MSFT-XVD`, enquanto os originais usam `msft-xvd`. A CLI Windows também comparava argumentos largos com literais estreitos e não compilava; a correção preserva caminhos Unicode.

Validação local: Windows x64, MSVC 19.44, Release; quatro grupos CTest de inspeção, inventário, ajuda e integração ZIP/CLI. Fixtures sintéticas cobrem SHA-256, limites, imports ausentes/ambíguos, erros, Unicode e JSON. Há casos para symlinks e junctions, mas o ambiente local recusou sua criação; esses casos foram pulados neste host, sem validação local desse comportamento. Linux/macOS permanecem para CI; não foram executados localmente. Os testes não comprovam runtime Xbox.

O build local exigiu debug information Embedded (`/Z7`) devido a C1902 do serviço PDB. A saída localizada de `/showIncludes` também impediu dependências incrementais corretas neste ambiente Ninja; a validação final usou uma compilação limpa, sem reutilizar objetos com layout antigo. Não houve mudança global da instalação do usuário.

Após compilar o inspetor, reproduzir a análise:

```powershell
python tools/study_update_zip.py 'C:\Users\fabeg\Downloads\OSU1.zip' 'C:\Users\fabeg\Downloads\OSU2.zip' 'C:\Users\fabeg\Downloads\OSU3.zip' --inspector build-checked/xone-native.exe --hash-archives --hash-members > osu-metadata.json
```

Omitir as opções de hash limita a leitura aos prefixos. Isso não verifica CRC ou integridade dos membros completos. Tamanhos vêm do diretório ZIP; nenhum executável de dentro do arquivo é iniciado. O programa externo chamado é somente o inspetor local explicitamente indicado.

Próxima dependência concreta: acesso permitido e legível aos executáveis e recursos de **um** conjunto coerente de SystemOS, identificação do shell e levantamento de imports/ativação. Os OSUs fornecem identidades e candidatos reais, mas ainda não um shell executável acessível ao projeto. Não foi escolhida uma versão para execução sem esse levantamento.
