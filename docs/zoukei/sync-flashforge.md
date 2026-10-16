# Sincronia Flashforge (ZMOD + IFS): Orca, HelixScreen, Moonraker

## Fontes de verdade
- Cor/material do slot: firmware ZMOD (`zmod_color`), escrito com `CHANGE_ZCOLOR` + `prompt_end`.
- Metadados (vendor, nome, temperaturas, `spool_id`): Moonraker DB `lane_data/lane<N>` (o HelixScreen le).
- Carretel: Spoolman (servidor em `/server/config` -> `spoolman.server`); carretel ativo em `/server/spoolman/spool_id`.

## Fluxos
1. Orca -> impressora: mudanca de filamento do projeto (auto-sync, 2 s), dialogo AMS da barra lateral ou "Editar slot" -> firmware + `lane_data`. Se a cor/material mudou, o `spool_id` da lane e solto. "Editar slot" tambem atualiza os filamentos do projeto mapeados ao slot (cor e preset), sem eco de volta (a linha de base do auto-sync e refeita).
2. Impressora -> Orca: o agente observa `zmod_color`/`lane_data`; se um slot mudou e o filamento mapeado difere, aparece a notificacao "Slot IFS n mudou na impressora" com o link "Sincronizar". Preferencias: "Aplicar mudancas de slot da impressora no projeto automaticamente" (padrao desligado).
3. Carretel ativo: com Orca aberto, quando o canal em uso (chaves `current_channel`/`channel`/`chan`/... de `zmod_ifs`; NAO VERIFICADO em hardware) tem `spool_id` diferente do ativo no Moonraker, o agente faz `POST /server/spoolman/spool_id`. Parado: sempre; imprimindo: so na troca de canal. Sem Orca: ver `ifs-spoolman-macro.cfg` (nao instalado).
4. "Sincronizar tudo agora" (aba IFS do painel): `lane_data` segue o firmware, reavalia o carretel ativo e o projeto segue os slots; mostra o que mudou.

## HelixScreen nao redesenhava o slot (causa)
O HelixScreen carrega `lane_data` so no inicio (`AmsBackendAd5xIfs::on_started`, ad5x.cpp:241-250). Depois, um campo "declarado" (`helix_declared`/`helix_locked_*`) vale mais que o firmware (spec.md secao 5) e o espelho do firmware pula campos declarados (`sync_override_to_firmware_locked`, ad5x.cpp:1496-1545). Nossos registros declaravam cor/material, entao a leitura nova de `GET_ZCOLOR` nao trocava a tela ate reiniciar. Agora gravamos registro "estrangeiro": sem `helix_*`, `vendor`, `spool_name`, `scan_time`; so `vendor_name`/`name`. Aviso: a declaracao ja em memoria do HelixScreen so some com um reinicio dele (uma vez).
