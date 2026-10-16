# Changelog

Todas as mudanças relevantes do PSTorrent5 serão registradas neste arquivo.

## [1.1.0] - 2026-10-16

Fork de [jfcardososantos/PS5Torrent](https://github.com/jfcardososantos/PS5Torrent),
renomeado para PSTorrent5 e com o motor corrigido.

### Incluído

- ps5-payload-sdk atualizado de v0.41 para **v0.44** (o SIGILL em firmware 13.60
  do issue upstream fica resolvido);
- identidade própria: title ID **PPSA91289**, content ID
  `IV9999-PPSA91289_00-PSTORRENT5000000` e binário `pstorrent5.elf`;
- painel web em **três idiomas** (espanhol, inglês e português) com detecção
  automática, seletor e verificação de traduções na CI (`tools/check-i18n.js`);
- códigos de erro estáveis (`error_code`) na API; o painel traduz o código e o
  servidor responde em inglês (klog e fallback);
- **correção crítica de rede**: o announce corre no mesmo thread do servidor
  web e o `connect()` a cada peer era bloqueante e sem timeout, deixando o
  painel congelado ao adicionar um torrent. Agora o connect é não bloqueante
  com timeout (5 s tracker / 1 s peers) e há um orçamento de conexões por
  announce;
- **duas bombas de pilha eliminadas**: `piece_buf[256 KB]` em `process_peer()`
  (agora `static`) e `data_b64[1 MB]` no upload de `.torrent`;
- build nativo em Windows sem WSL/sudo/make (`build.ps1` + shim
  `tools/prospero-lld`), verificado e documentado em `docs/BUILD-WINDOWS.md`;
- `ARCHITECTURE.md` e `TODO.md` com a arquitetura objetivo, as restrições
  técnicas e o plano por hitos.

### Notas

- O motor BitTorrent continua sendo o do upstream: apenas trackers HTTP, sem
  DHT, sem magnet (BEP-9) e sem reanudação. Está no plano (`TODO.md`).
- O código-fonte segue sob GPL-3.0 e mantém os avisos de autoria do projeto
  original.

## [1.0.0] - 2026-07-21

Primeira versão pública.

### Incluído

- cliente BitTorrent em C para PS5;
- painel web responsivo com progresso, velocidade, ETA e peers;
- upload de arquivos `.torrent` e seleção de armazenamento;
- abertura automática do navegador e notificações do sistema;
- diagnóstico de rede pelo macOS;
- payload ELF compilado com ps5-payload-sdk v0.41;
- fPKG experimental com ícone próprio na tela principal;
- geração do ELF e do fPKG inteiramente pelo macOS;
- build automatizado no GitHub Actions.
