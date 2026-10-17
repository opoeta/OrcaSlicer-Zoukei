# Spoolman no Orca Zoukei (somente leitura)

O Orca nunca cria nem edita nada no Spoolman: so `GET /api/v1/...` (info, spool, spool/<id>).

## Servidor
- Preferencias > Recursos > "Servidor Spoolman" (`spoolman_url` no app config). Botoes Testar / Detectar.
- Vazio: preenchido com `spoolman.server` de `GET /server/config` de uma impressora Moonraker conectada (agente Flashforge/ZMOD ao conectar, ou o botao Detectar).

## Carretel por filamento (projeto)
- Opcao de projeto `filament_spool_id` (coStrings, "" = nenhum), salva no 3MF, fora do G-code (como `filament_cfs_slot`), alinhada em `PresetBundle::normalize_filament_colour_lists`.
- Remote API: `set_config`/`get_config` aceitam `filament_spool_id` ("12;;3;;;;;", um item por filamento, vazio ou inteiro positivo).
- Escolha: botao direito no cracha/caixa de cor do filamento, ou menu "..." do filamento > "Carretel Spoolman". Dialogo com cor (inclui `multi_color_hexes`), fornecedor, nome, material, restante, local e slot.
- Ao escolher: cor do projeto (coaxial -> varias cores lado a lado, longitudinal -> gradiente), melhor preset (fornecedor + material + nome, senao o generico do material) e o id.

## Carretel por slot
- Flashforge/ZMOD: `lane_data/lane<n>.spool_id` (+ `spoolman_vendor_id`, `helix_spoolman_filament_id`, `vendor_name`, `name`). O HelixScreen le o resto no Spoolman. Um carretel fica em uma lane so. O carretel ativo do Moonraker segue o canal carregado (`sync_active_spool`). Registro "estrangeiro" (ver sync-flashforge.md).
- Creality K2 e Bambu: vinculo local no app config, secao `spoolman_slots`, chave `<impressora>:<box>:<slot>`.
- Leitura de volta: o cracha mostra o carretel (do projeto, ou o do slot) na dica; o seletor de slot mostra `#id`; o dialogo de carretel mostra a coluna Slot.

## Verificacao antes de imprimir
- Dialogos de envio (SelectMachine, Flashforge, base PrintHost): se o `remaining_weight` do carretel vinculado for menor que o peso fatiado do filamento (volume x densidade), avisa.
