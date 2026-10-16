<p align="center">
  <img src="pkg/sce_sys/pic1.png" alt="PSTorrent5" width="100%">
</p>

<h1 align="center">PSTorrent5</h1>

<p align="center">
  Um cliente BitTorrent para PS5 com painel web, acompanhamento em tempo real
  e aplicativo próprio na tela principal do console.
</p>

<p align="center">
  <a href="https://github.com/LiMPbIzK/PSTorrent5/releases"><img alt="Versão" src="https://img.shields.io/badge/versão-1.1.0-7c5cff?style=for-the-badge"></a>
  <a href="https://github.com/LiMPbIzK/PSTorrent5/actions/workflows/build.yml"><img alt="Build" src="https://img.shields.io/github/actions/workflow/status/LiMPbIzK/PSTorrent5/build.yml?branch=main&style=for-the-badge&label=build"></a>
  <img alt="Plataforma" src="https://img.shields.io/badge/plataforma-PS5-006FCD?style=for-the-badge&logo=playstation">
  <img alt="Linguagem" src="https://img.shields.io/badge/linguagem-C-14b8a6?style=for-the-badge&logo=c">
  <img alt="Licença" src="https://img.shields.io/badge/licença-GPL--3.0-f59e0b?style=for-the-badge">
</p>

<p align="center">
  <a href="#downloads">Downloads</a> •
  <a href="#recursos">Recursos</a> •
  <a href="#como-usar">Como usar</a> •
  <a href="#compilação">Compilação</a>
</p>

> [!IMPORTANT]
> Projeto experimental destinado a pesquisa e homebrew. Use somente torrents
> de conteúdo que você tem autorização para baixar. Requer um PS5 com ambiente
> de homebrew compatível; não utilize uma conta PSN importante durante testes.

> [!NOTE]
> **Fork.** Este projeto é um fork de
> [jfcardososantos/PS5Torrent](https://github.com/jfcardososantos/PS5Torrent)
> (GPL-3.0). O motor BitTorrent e o painel originais são do autor upstream.
> Este fork acrescenta o ps5-payload-sdk v0.44, correções de rede (conexões
> com timeout) e de pilha, o painel em três idiomas (es/en/pt) e o renomeado
> para PSTorrent5. Estado atual e decisões técnicas: `ARCHITECTURE.md` e
> `TODO.md`.

## Downloads

A versão 1.1 é distribuída de duas formas:

| Arquivo | Quando usar |
| --- | --- |
| `PSTorrent5-v1.1.0.pkg` | Instala o PSTorrent5 com ícone próprio na tela principal. |
| `pstorrent5-v1.1.0.elf` | Envio direto para um ELF loader nas portas 9021/9020. |
| Source code | Código completo em `.zip` ou `.tar.gz`, gerado pelo GitHub. |

Os binários ficam nos assets da página
**[Releases](https://github.com/LiMPbIzK/PSTorrent5/releases)**.
O fPKG é uma
imagem debug experimental e sua instalação depende do firmware e do ambiente
homebrew utilizado no console.

## Recursos

- painel responsivo preparado para TV, navegador do PS5 e controle;
- progresso, velocidade, tamanho restante, ETA e peers em tempo real;
- upload de arquivos `.torrent` pelo navegador;
- metainfo single-file e multi-file;
- trackers HTTP, protocolo BitTorrent peer wire e verificação SHA-1 das peças;
- seleção de armazenamento USB, M.2/NVMe ou interno disponível;
- notificação do sistema com o endereço do painel;
- abertura automática de `http://127.0.0.1:12389/` no PS5;
- fPKG com ícone e arte próprios para a tela principal;
- diagnóstico do console diretamente pelo macOS;
- build reproduzível com o ps5-payload-sdk v0.44.

<p align="center">
  <img src="pkg/sce_sys/pic0.png" alt="Arte do painel PSTorrent5" width="88%">
</p>

## Como usar

### Aplicativo instalado

Instale o `.pkg` usando o instalador do seu ambiente homebrew. Ao abrir o
ícone **PSTorrent5** na tela principal, o aplicativo inicia o serviço na porta
12389, exibe uma notificação e abre o painel local no navegador.

### Payload ELF

Com um loader ouvindo na porta 9021:

```bash
export PS5_HOST=192.168.1.100
export PS5_PORT=9021
make test
```

O ELF pronto fica na raiz do projeto como `pstorrent5.elf`. No aplicativo do
loader, escolha esse arquivo; não é necessário copiar outros arquivos do
repositório. Quando ele iniciar, o painel abre automaticamente no PS5. Se o
navegador não abrir, acesse `http://IP_DO_PS5:12389/` em qualquer aparelho da
mesma rede.

Também é possível selecionar `pstorrent5.elf` em um aplicativo de envio de
payloads. Depois do carregamento, acesse:

```text
http://IP_DO_PS5:12389/
```

Para confirmar pelo Mac o que está respondendo:

```bash
./scripts/check_ps5.sh 192.168.1.100
```

O diagnóstico testa o painel na porta 12389 e os loaders nas portas 9021 e
9020. Quando o serviço está ativo, o estado da API é mostrado no terminal.

## Compilação

### Windows (nativo)

É possível compilar o ELF em Windows sem WSL, sem `sudo` e sem `make`:

```powershell
.\build.ps1
```

Ver `docs/BUILD-WINDOWS.md` para requisitos, shim do enlazador e validação.

### macOS

Pré-requisitos:

- macOS com Command Line Tools (`xcode-select --install`);
- [Homebrew](https://brew.sh).

Prepare o SDK e compile o ELF:

```bash
./setup.sh
```

O script instala/verifica `llvm@18` e `socat`, baixa o ps5-payload-sdk v0.44,
confere seu SHA-256 e gera `pstorrent5.elf`. Tudo permanece dentro do projeto,
sem `sudo` e sem alterações no `.zshrc`.

Comandos adicionais:

```bash
make                       # recompila o ELF
make clean all             # build limpo
./setup.sh --no-deps       # não executa brew install
./setup.sh --sdk-only      # prepara apenas o SDK
```

### Gerar o fPKG no Mac

Não é necessário Windows. Instale o .NET 10 e execute:

```bash
brew install dotnet
./scripts/build_pkg_macos.sh
```

O pacote é gerado em `dist/` usando a
[LibProsperoPKG](https://github.com/SvenGDK/LibProsperoPKG) v2.5 com uma camada
SHA3-256 portátil para macOS. A revisão da dependência é fixada e validada pelo
script. Consulte [pkg/TILE.md](pkg/TILE.md) para mais detalhes.

Para que o fPKG possa gravar no USB e em outros pontos fora do sandbox, ative
as opções **Network** e **Legacy CMD server** no toolbox do etaHEN antes de
abrir o PSTorrent5. O aplicativo solicita a liberação automaticamente ao
iniciar. O ELF enviado diretamente ao loader continua funcionando sem esse
serviço quando já tiver acesso ao sistema de arquivos.

## Limitações da versão 1.0

- magnet links ainda não baixam metadados BEP-9; use arquivos `.torrent`;
- trackers HTTPS/UDP, DHT, PEX, MSE/PE e retomada não estão implementados;
- o painel não possui autenticação e deve ficar em uma rede local confiável;
- compatibilidade do fPKG varia conforme firmware, jailbreak e instalador;
- a versão ainda precisa de validação mais ampla em hardware real.

## Estrutura do projeto

```text
.
├── .github/workflows/     # integração contínua
├── include/               # headers C
├── src/                   # cliente, servidor HTTP e painel incorporado
│   └── web/index.html     # fonte editável da interface
├── pkg/                   # metadados, ícone e artes do aplicativo
├── scripts/               # build, diagnóstico e utilitários
├── tools/                 # empacotador fPKG para macOS
├── Makefile               # build do payload
└── setup.sh               # preparação reproduzível do ambiente
```

## Segurança e uso responsável

O PSTorrent5 não inclui conteúdo, catálogos ou mecanismos de busca. O usuário é
responsável pelos torrents adicionados e pelo cumprimento das leis e licenças
aplicáveis. Homebrew pode violar os termos de serviço da Sony e expor o console
a riscos.

## Licença

Distribuído sob a licença [GPL-3.0](LICENSE).
