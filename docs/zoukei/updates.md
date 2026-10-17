# Atualizações do Orca Zoukei (Windows)

O Orca Zoukei se atualiza a partir das *releases* do GitHub `opoeta/OrcaZoukei`, baixando só o que mudou quando possível.
Nunca consulta as releases do OrcaSlicer oficial (o código de verificação do upstream segue desligado desde `3b492d0`).

## Como funciona (usuário)

- **Ajuda > Verificar atualizações** (e Speed Dial) consulta o GitHub e mostra versão nova, notas e os botões
  **Atualizar** / **Depois** / **Ignorar esta versão**.
- **Preferências > Online > Atualização**: *Verificar atualizações do Orca Zoukei ao abrir* (padrão ligado, no máximo 1 vez por dia, desligado em modo furtivo) e
  *Canal beta* (inclui releases marcadas como pré-lançamento).
- **Atualizar**: confere os arquivos instalados, baixa o pacote (delta se der, senão o completo), verifica o SHA-256 de cada arquivo,
  e pede para reiniciar. Ao confirmar, o Orca fecha, o `zoukei-updater.exe` troca os arquivos (com backup e reversão automática) e abre o Orca de novo.
- A versão do build aparece em **Sobre** (`Orca Zoukei 2026.10.17`). Vem de `ORCA_ZOUKEI_VERSION` em `version.inc`
  (ou da tag `vX` quando o build é de um commit com tag).
- A pasta do usuário (AppData: presets, configurações, plugins) **não é tocada**: só os arquivos da pasta onde o `orca-slicer.exe` está.
  Arquivos que não estão no manifesto (ex.: algo que você colocou na pasta) são mantidos.
- Se o Orca está numa pasta sem permissão de escrita (ex.: `C:\Program Files`), o diálogo avisa e oferece abrir a página da release (troca manual).

## Velopack ou esquema próprio?

Escolhido: **esquema próprio mínimo**. Velopack (MIT, GitHub Releases, deltas, SDK C++) foi avaliado e descartado por agora:

| | Velopack | Próprio (este) |
|---|---|---|
| Modelo de instalação | Impõe `Setup.exe`/`%LocalAppData%\<app>\current`; o Orca do dono é uma pasta (`C:\OrcaSlicer`) | Atualiza a pasta onde já está (portátil ou instalada) |
| Ferramental de release | `vpk` (.NET) obrigatório no PC do dono e no CI | Um script Python só com a biblioteca padrão |
| Delta | Binário (bsdiff) por pacote, ótimo para um `OrcaSlicer.dll` grande | Por arquivo: só arquivos que mudaram (o `OrcaSlicer.dll` vai inteiro, o resto — 370 MB de `resources` — quase nunca muda) |
| Integração C++ | Precisa incorporar o SDK e a cadeia de build dele no CMake do Orca | poucas centenas de linhas, só libs que o Orca já usa (curl, OpenSSL, miniz, nlohmann) |
| Falhas / reversão | Do Velopack | Backup + reversão no `zoukei-updater.exe`, testado |
| Risco para o dono | Muda como o app é instalado, atalhos, desinstalador | Nenhum: continua a mesma pasta |

O ganho de bytes do bsdiff em relação a "arquivo inteiro que mudou" é pequeno aqui (no Orca quase tudo é um DLL grande + recursos estáticos).
Se um dia valer a pena, o manifesto já separa "o que baixar" de "o que aplicar", então trocar o formato do delta não muda o cliente.

## Assets de uma release

Para a plataforma `windows-x64` (nome fixo `<plat>`):

| Asset | Conteúdo |
|---|---|
| `manifest-<plat>.json` | `files`: `{caminho: [tamanho, sha256]}` de cada arquivo da versão; `full`: zip completo (nome, tamanho, sha256); `deltas`: lista com `from`, `asset`, `size`, `sha256`, `files` (o que o zip contém) e `removed` |
| `OrcaZoukei-<versão>-<plat>.zip` | árvore completa (também serve como instalação nova/portátil) |
| `delta-<de>-<para>-<plat>.zip` | só os arquivos que mudaram desde `<de>` (um por release anterior, até 3) |
| `SHA256SUMS.txt` | sums dos assets, para quem baixa na mão |

Versão: `AAAA.MM.DD` (ou `AAAA.MM.DD.N` para a 2ª release do dia); a tag é `v` + versão. Tag com `-` (ex.: `v2026.10.20-beta.1`) = pré-lançamento (canal beta).

## Escolha delta x completo (cliente)

1. Baixa o manifesto da release mais nova; calcula o SHA-256 de **todos** os arquivos instalados que o manifesto lista (~5 s).
2. `necessários` = arquivos ausentes/diferentes. Se existe um delta com `from == versão instalada` que contém todos os `necessários` e é menor que o completo, baixa o delta; senão baixa o completo.
3. Baixa em pedaços de 8 MB (`Range`, 4 tentativas por pedaço), confere o SHA-256 do zip, extrai **só** os `necessários`
   para `%TEMP%\OrcaZoukeiUpdate\<versão>\files`, conferindo tamanho e SHA-256 de cada um.
4. Remoções: o que estava na versão instalada (`zoukei-installed.json`, gravado na pasta do app a cada atualização) e não está na nova; para o delta, também `removed`.
5. Escreve `plan.txt` e o `zoukei-updater.exe` (cópia no temp) assume depois que o Orca sair.

## Integridade e segurança

- Só HTTPS, e só `https://api.github.com/repos/opoeta/OrcaZoukei/` (API) e `https://github.com/opoeta/OrcaZoukei/releases/download/` (assets). Qualquer outro endereço é recusado antes de qualquer conexão.
  O GitHub redireciona os assets para o CDN dele (também HTTPS); a verificação do certificado usa o repositório de certificados do Windows.
- Todo arquivo tem o SHA-256 conferido: o zip baixado, cada arquivo extraído e, de novo, cada arquivo preparado antes de tocar na instalação (o `zoukei-updater.exe` refaz a conta).
- Nenhum código baixado é executado antes de verificado, e o `zoukei-updater.exe` usado é o **já instalado** (nunca um baixado): a atualização seguinte é que pode trazer um novo.
- Caminhos do manifesto/plano: relativos, sem `..`, sem `:` nem `\`, sem nome terminando em ponto/espaço. Zip: só são extraídos os nomes do manifesto, com tamanho conferido (sem zip bomb).
- `zoukei-updater.exe`: espera o Orca sair (até 120 s), confere tudo, move cada arquivo substituído para um backup, copia o novo; qualquer falha (arquivo travado por 30 s, disco cheio...) reverte
  tudo do backup e abre a versão antiga com um aviso. Log: `%TEMP%\OrcaZoukeiUpdate\<versão>\update.log`.
- **Limite conhecido:** a confiança é HTTPS + conta/repositório do dono no GitHub. Não há assinatura própria do manifesto (quem consegue publicar uma release consegue publicar um manifesto).
  Se isso virar requisito, o caminho é assinar o `manifest` (minisign/Ed25519) e embutir a chave pública no app.
- Modo furtivo desliga a verificação ao abrir (a manual do menu continua funcionando).

## Publicar uma release (dono)

Local (recomendado na primeira vez):

```powershell
# 1. build normal + instalar + regenerar o .mo (run-compile / run-install / po2mo), versão em version.inc igual à tag
# 2. empacotar (só lê a árvore e, com --fetch-previous, os manifestos das releases anteriores)
python tools\zoukei\make_release.py --tree build\OrcaSlicer --version 2026.10.17 --fetch-previous 3 --notes-file notas.md
# 3. o script imprime o comando exato; confira e rode você mesmo:
gh release create "v2026.10.17" --repo opoeta/OrcaZoukei --title "Orca Zoukei 2026.10.17" --target <commit> --notes-file notas.md <assets...>
```

O commit alvo precisa estar no GitHub (`git push zoukei k2-wave-main:main`). O script não publica nada.
Marque `--prerelease` (e a tag com `-beta.N`) para o canal beta. A primeira release não tem deltas; as seguintes passam a ter.

Pelo GitHub Actions: `git tag v2026.10.17 && git push zoukei v2026.10.17` dispara `.github/workflows/zoukei-release.yml`
(build no `windows-latest`, mesmo empacotamento, publica a release). O build completo no CI **não foi exercitado de ponta a ponta** (dependências, ~1 h na primeira vez, depois cache);
se der problema, o caminho local acima continua valendo.

## Testes sem GitHub

```powershell
python tools\zoukei\test_zoukei_tools.py --updater build\OrcaSlicer\zoukei-updater.exe   # make_release + updater (aplicar, hash errado, caminho inseguro, arquivo travado/reversão, espera do processo)
python tools\zoukei\fake_release_server.py --port 8099 dist\release-A dist\release-B      # falsa API + assets em 127.0.0.1 (com Range)
```

Teste de ponta a ponta com o app de verdade (copia a árvore, usa `--datadir` temporário, servidor falso, sem GitHub e sem impressoras):

```powershell
python tools\zoukei\e2e_update_test.py --tree build\OrcaSlicer --work D:\scratch\zoukei-e2e A B C D
```

`A` delta, `B` delta corrompido (nada pode mudar), `C` pacote completo, `D` abre o diálogo real e clica nele. Os ganchos de teste são variáveis de ambiente que só valem junto com
`ORCA_ZOUKEI_UPDATE_URL=http://127.0.0.1:<porta>/releases.json` (só aceita http em `127.0.0.1`/`localhost`; sem ela só o GitHub do dono é contatado): `ORCA_ZOUKEI_UPDATE_AUTO=1|dialog`. Nesse modo o updater não reabre o Orca e o app não registra o `orcaslicer://`. Rode sempre com `--datadir` temporário, nunca com o AppData real.
