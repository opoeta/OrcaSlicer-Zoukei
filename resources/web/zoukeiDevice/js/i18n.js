// Texts. The language comes from Orca (get_lang): Portuguese for any pt_*, English otherwise.
// Add a language by adding a column here and a case in languageFor().
(function (Z) {
    'use strict';

    const T = {
        en: {
            printers: 'Printers', add: 'Add', added: 'Added', scan_lan: 'Scan network', scanning: 'Searching the network...',
            scan_found: '{n} printer(s) found', scan_none: 'No Creality printer answered. Check that the printer is on and on the same network.',
            ip_address: 'Printer IP address', list_empty: 'No printers yet. Type an IP address above or scan the network.',
            err_bad_ip: 'Enter a valid IPv4 address.', err_not_lan: 'Only addresses on your local network can be added.',
            err_probe: 'Could not reach a Creality printer at that address.', err_scan: 'The network scan failed.',
            checking: 'Checking...', online: 'Online', offline: 'Offline', connecting: 'Connecting...',
            rename: 'Rename', remove: 'Remove', more: 'More', name: 'Name', save: 'Save', cancel: 'Cancel', close: 'Close', ok: 'OK',
            confirm: 'Confirm', remove_confirm: 'Remove {name} from the list?', loading: 'Loading...', refresh: 'Refresh',
            phase_idle: 'Idle', phase_printing: 'Printing', phase_paused: 'Paused', phase_pausing: 'Pausing', phase_done: 'Finished',
            phase_stopped: 'Stopped', phase_restoring: 'Recovering',
            camera: 'Camera', fullscreen: 'Fullscreen', camera_connecting: 'Connecting to the camera...', camera_unavailable: 'Camera unavailable', camera_lost: 'Camera connection lost', camera_reconnect: 'Reconnect camera',
            no_job: 'No print job', pause: 'Pause', resume: 'Resume', stop: 'Stop', stop_confirm: 'Stop the current print? This cannot be resumed.',
            layer: 'Layer', elapsed: 'Elapsed', remaining: 'Remaining', printer_error: 'Printer error',
            nozzle: 'Nozzle', bed: 'Bed', chamber: 'Chamber', current: 'Current', target: 'Target', off: 'Off', max_temp: 'Max {v} °C',
            led: 'LED', fan_model: 'Model fan', fan_case: 'Case fan', fan_aux: 'Side fan',
            speed: 'Print speed', speed_silent: 'Silent', speed_stable: 'Stable', speed_standard: 'Standard', speed_ultra: 'Ultra fast',
            tab_cfs: 'Filament (CFS)', tab_move: 'XYZ control', tab_files: 'Files', tab_history: 'History',
            ext_spool: 'External spool', ext_short: 'Ext', slot: 'Slot', slot_empty: 'Empty', cfs_loading: 'Reading the CFS...', cfs_none: 'No CFS data.',
            cfs_pick: 'Select a slot, then use Edit, Feed or Retract.', cfs_locked: 'Locked while a print is running.', cfs_moving: 'Filament is moving, please wait.',
            auto_refill: 'Auto refill', auto_refill_info: 'Slots with the same material and colour take over from each other when one runs out.', auto_refill_none: 'No slots share the same material and colour.', auto_refill_on: 'Auto refill on',
            tab_timelapse: 'Timelapse', download: 'Download', video_none: 'No timelapse videos on the printer.', video_saved: 'Video saved to {path}', video_failed: 'Could not save the video',
            history_chart: 'Temperature history', hist_chart_title: 'Temperature history (last 10 minutes)', chart_wait: 'Collecting data...', edit_slot: 'Edit slot', feed: 'Feed', retract: 'Retract', reread_rfid: 'Re-read RFID',
            rfid_readonly: 'This slot holds a Creality RFID spool and is read-only.', type: 'Type', vendor: 'Vendor', temp_range: 'Temperature',
            mode_preset: 'Orca filament preset', mode_custom: 'Custom', search_filament: 'Search filament presets', all_types: 'All types',
            presets_loading: 'Loading presets...', presets_none: 'No matching preset.', pick_preset: 'Pick a filament preset first.',
            library_loading: 'Reading the printer material library...', color: 'Color', temp_min: 'Min. temperature (°C)', temp_max: 'Max. temperature (°C)',
            pressure_advance: 'Pressure advance', clear_slot: 'Clear slot', clear_confirm: 'Clear slot {slot}? The printer will forget its material.',
            err_type: 'Enter the material type.', err_temps: 'Temperatures must be between 100 and 450 °C, minimum below maximum.',
            err_send: 'The printer is not connected.', slot_saved: 'Slot updated.', slot_unconfirmed: 'The printer did not confirm the change.',
            home_all: 'Home all', position: 'Position', move_locked: 'Movement is locked while a print is running.',
            search_files: 'Search files', print: 'Print', delete: 'Delete', files_none: 'No files on the printer.',
            print_confirm: 'Start printing {name}?', delete_confirm: 'Delete {name} from the printer?',
            hist_jobs: 'Prints', hist_time: 'Print time', hist_material: 'Filament used', hist_none: 'No history yet.',
            hist_done: 'Finished', hist_incomplete: 'Interrupted'
        },
        pt: {
            printers: 'Impressoras', add: 'Adicionar', added: 'Adicionada', scan_lan: 'Buscar na rede', scanning: 'Procurando na rede...',
            scan_found: '{n} impressora(s) encontrada(s)', scan_none: 'Nenhuma impressora Creality respondeu. Confira se está ligada e na mesma rede.',
            ip_address: 'Endereço IP da impressora', list_empty: 'Nenhuma impressora ainda. Digite um endereço IP acima ou busque na rede.',
            err_bad_ip: 'Digite um endereço IPv4 válido.', err_not_lan: 'Só é possível adicionar endereços da sua rede local.',
            err_probe: 'Não foi possível alcançar uma impressora Creality nesse endereço.', err_scan: 'A busca na rede falhou.',
            checking: 'Verificando...', online: 'Online', offline: 'Offline', connecting: 'Conectando...',
            rename: 'Renomear', remove: 'Remover', more: 'Mais', name: 'Nome', save: 'Salvar', cancel: 'Cancelar', close: 'Fechar', ok: 'OK',
            confirm: 'Confirmar', remove_confirm: 'Remover {name} da lista?', loading: 'Carregando...', refresh: 'Atualizar',
            phase_idle: 'Ociosa', phase_printing: 'Imprimindo', phase_paused: 'Pausada', phase_pausing: 'Pausando', phase_done: 'Concluída',
            phase_stopped: 'Interrompida', phase_restoring: 'Recuperando',
            camera: 'Câmera', fullscreen: 'Tela cheia', camera_connecting: 'Conectando à câmera...', camera_unavailable: 'Câmera indisponível', camera_lost: 'Conexão da câmera perdida', camera_reconnect: 'Reconectar câmera',
            no_job: 'Nenhuma impressão', pause: 'Pausar', resume: 'Retomar', stop: 'Parar', stop_confirm: 'Parar a impressão atual? Não será possível retomar.',
            layer: 'Camada', elapsed: 'Decorrido', remaining: 'Restante', printer_error: 'Erro da impressora',
            nozzle: 'Bico', bed: 'Mesa', chamber: 'Câmara', current: 'Atual', target: 'Alvo', off: 'Desligar', max_temp: 'Máx. {v} °C',
            led: 'LED', fan_model: 'Ventilador do modelo', fan_case: 'Ventilador do gabinete', fan_aux: 'Ventilador lateral',
            speed: 'Velocidade de impressão', speed_silent: 'Silencioso', speed_stable: 'Estável', speed_standard: 'Padrão', speed_ultra: 'Ultrarrápido',
            tab_cfs: 'Filamento (CFS)', tab_move: 'Controle XYZ', tab_files: 'Arquivos', tab_history: 'Histórico',
            ext_spool: 'Suporte externo', ext_short: 'Ext', slot: 'Slot', slot_empty: 'Vazio', cfs_loading: 'Lendo o CFS...', cfs_none: 'Sem dados do CFS.',
            cfs_pick: 'Selecione um slot e use Editar, Alimentar ou Retrair.', cfs_locked: 'Bloqueado durante a impressão.', cfs_moving: 'O filamento está em movimento, aguarde.',
            auto_refill: 'Reabastecimento automático', auto_refill_info: 'Slots com o mesmo material e cor se revezam quando um acaba.', auto_refill_none: 'Nenhum slot compartilha o mesmo material e cor.', auto_refill_on: 'Reabastecimento automático ligado',
            tab_timelapse: 'Timelapse', download: 'Baixar', video_none: 'Nenhum vídeo timelapse na impressora.', video_saved: 'Vídeo salvo em {path}', video_failed: 'Não foi possível salvar o vídeo',
            history_chart: 'Histórico de temperatura', hist_chart_title: 'Histórico de temperatura (últimos 10 minutos)', chart_wait: 'Coletando dados...', edit_slot: 'Editar slot', feed: 'Alimentar', retract: 'Retrair', reread_rfid: 'Reler RFID',
            rfid_readonly: 'Este slot tem um carretel RFID da Creality e é somente leitura.', type: 'Tipo', vendor: 'Fabricante', temp_range: 'Temperatura',
            mode_preset: 'Preset de filamento do Orca', mode_custom: 'Personalizado', search_filament: 'Buscar presets de filamento', all_types: 'Todos os tipos',
            presets_loading: 'Carregando presets...', presets_none: 'Nenhum preset encontrado.', pick_preset: 'Escolha um preset de filamento primeiro.',
            library_loading: 'Lendo a biblioteca de materiais da impressora...', color: 'Cor', temp_min: 'Temperatura mín. (°C)', temp_max: 'Temperatura máx. (°C)',
            pressure_advance: 'Avanço de pressão', clear_slot: 'Limpar slot', clear_confirm: 'Limpar o slot {slot}? A impressora esquecerá o material.',
            err_type: 'Informe o tipo de material.', err_temps: 'As temperaturas devem ficar entre 100 e 450 °C, com a mínima abaixo da máxima.',
            err_send: 'A impressora não está conectada.', slot_saved: 'Slot atualizado.', slot_unconfirmed: 'A impressora não confirmou a alteração.',
            home_all: 'Home geral', position: 'Posição', move_locked: 'Movimentos bloqueados durante a impressão.',
            search_files: 'Buscar arquivos', print: 'Imprimir', delete: 'Apagar', files_none: 'Nenhum arquivo na impressora.',
            print_confirm: 'Iniciar a impressão de {name}?', delete_confirm: 'Apagar {name} da impressora?',
            hist_jobs: 'Impressões', hist_time: 'Tempo de impressão', hist_material: 'Filamento usado', hist_none: 'Sem histórico ainda.',
            hist_done: 'Concluída', hist_incomplete: 'Interrompida'
        }
    };

    function languageFor(code) {
        return /^pt/i.test(String(code || '')) ? 'pt' : 'en';
    }

    const handlers = [];
    let lang = 'en';
    let explicit = false;

    Z.lang = lang;
    Z.t = function (key, vars) {
        let s = (T[lang] && T[lang][key]) || T.en[key] || key;
        if (vars) s = s.replace(/\{(\w+)\}/g, (m, k) => (k in vars ? vars[k] : m));
        return s;
    };
    Z.setLanguage = function (code) {
        const next = languageFor(code);
        const changed = next !== lang || !explicit;
        lang = next;
        explicit = true;
        Z.lang = lang;
        document.documentElement.lang = lang === 'pt' ? 'pt-BR' : 'en';
        if (changed) handlers.slice().forEach(fn => fn(lang));
    };
    Z.onLanguage = function (fn) { handlers.push(fn); };
})(window.Z = window.Z || {});
