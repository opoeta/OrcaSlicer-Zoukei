// K2 / Creality error table for the device panel.
//
// Source: Creality Print (AGPL-3.0), resources/web/deviceMgr/assets (errorJson + printerCode tables, the
// code -> English message map and the Creality error codes such as "AC0500"). Messages are extracted
// unchanged (whitespace/quotes cleaned); the Brazilian Portuguese texts and the per-family hints are ours.
// Same lookup rule as Creality's getErrorLookupKey(): a string errcode found in the table wins, otherwise
// the numeric `key` is used. Moonraker/Flashforge synthetic errors (errcode 1/2 with no `key`) are not
// looked up here: Z.k2Error() returns null for them so detail.js keeps its old text.
(function (Z) {
    'use strict';

    // level: 'error' (stops/needs attention) or 'warning' (recoverable, Creality level 1/3). cc: Creality code.
    const E = {
        0: {"en": "An unknown error was detected in printing", "pt_BR": "Erro desconhecido detectado durante a impressão", "level": "error"},
        1: {"en": "Motor drive abnormality", "pt_BR": "Anormalidade no acionamento do motor", "level": "error"},
        2: {"en": "internal error", "pt_BR": "Erro interno", "level": "error"},
        3: {"en": "Communication abnormality", "pt_BR": "Falha de comunicação", "level": "error"},
        4: {"en": "Not heating as expected", "pt_BR": "Aquecimento fora do esperado", "level": "error"},
        6: {"en": "Extruder abnormality", "pt_BR": "Anormalidade no extrusor", "level": "error"},
        7: {"en": "Print file coordinates are abnormal", "pt_BR": "Coordenadas do arquivo de impressão anormais", "level": "error"},
        9: {"en": "Chatter optimization sensor abnormality", "pt_BR": "Anormalidade no sensor de otimização de vibração (chatter)", "level": "error"},
        10: {"en": "Fan abnormality", "pt_BR": "Anormalidade no ventilador", "level": "error"},
        100: {"en": "An unknown error was detected in printing", "pt_BR": "Erro desconhecido detectado durante a impressão", "level": "error"},
        101: {"en": "A print quality issue has been detected and printing has been paused", "pt_BR": "Problema de qualidade de impressão detectado; impressão pausada", "level": "warning", "cc": "AC0101"},
        104: {"en": "Foreign object detected on the printing platform, printing paused", "pt_BR": "Objeto estranho detectado na plataforma de impressão; impressão pausada", "level": "warning", "cc": "AC0104"},
        109: {"en": "Abnormal heat break fan", "pt_BR": "Ventilador do heat break anormal", "level": "error", "cc": "CF0109"},
        110: {"en": "Motherboard Connection Failure", "pt_BR": "Falha de conexão com a placa-mãe", "level": "error", "cc": "BM0110"},
        111: {"en": "Nozzle connection abnormal", "pt_BR": "Conexão do bico anormal", "level": "error", "cc": "BM0111"},
        112: {"en": "Hotbed Connection Abnormal", "pt_BR": "Conexão da mesa aquecida anormal", "level": "error", "cc": "BM0112"},
        114: {"en": "CrealityCloud G-code download exception.", "pt_BR": "Falha no download do G-code da CrealityCloud", "level": "warning", "cc": "XD0114"},
        115: {"en": "Detected unfinished task, do you wish to continue printing", "pt_BR": "Tarefa inacabada detectada; deseja continuar imprimindo?", "level": "warning", "cc": "CM0115"},
        116: {"en": "Filament runout, printing paused, please refill", "pt_BR": "Filamento acabou, impressão pausada; recarregue o filamento", "level": "warning", "cc": "TR0116"},
        117: {"en": "A model warping issue has been detected,printing has been paused", "pt_BR": "Empenamento (warping) do modelo detectado; impressão pausada", "level": "warning", "cc": "AC0117"},
        119: {"en": "Detected waste chute is block. Please clean it up and try again.", "pt_BR": "Calha de descarte entupida. Limpe e tente novamente.", "level": "warning", "cc": "AC0119"},
        120: {"en": "Belt tension abnormal. Please rerun the Input Shaping before using the printer.", "pt_BR": "Tensão da correia anormal. Refaça o Input Shaping antes de usar a impressora.", "level": "warning", "cc": "CA0120"},
        121: {"en": "FR2357: CFS filament in use. To print with spool holder, unload CFS filament and reload spool holder before printing.", "pt_BR": "FR2357: filamento do CFS em uso. Para imprimir com o suporte de carretel, descarregue o filamento do CFS e carregue o do suporte antes de imprimir.", "level": "warning", "cc": "FR0121"},
        122: {"en": "FR2358: Spool holder filament in use. To print with CFS, unload spool holder filament and reload CFS filament before printing.", "pt_BR": "FR2358: filamento do suporte de carretel em uso. Para imprimir com o CFS, descarregue o filamento do suporte e recarregue o do CFS antes de imprimir.", "level": "warning", "cc": "FR0122"},
        123: {"en": "Detected printing plate is missing, printing paused", "pt_BR": "Placa de impressão ausente; impressão pausada", "level": "error", "cc": "AC0123"},
        124: {"en": "Detected printing plate is missing and foreign object on the printing plate, printing paused", "pt_BR": "Placa de impressão ausente e objeto estranho sobre ela; impressão pausada", "level": "error", "cc": "AC0124"},
        125: {"en": "The current version doesn't support T command desassembled G-code files. Please upgrade the firmware.", "pt_BR": "Esta versão não suporta G-code desmontado com comando T. Atualize o firmware.", "level": "warning", "cc": "XD0125"},
        201: {"en": "Abnormal temperature of the temperature chamber", "pt_BR": "Temperatura anormal da câmara aquecida", "level": "error"},
        204: {"en": "Fan abnormality", "pt_BR": "Anormalidade no ventilador", "level": "error"},
        205: {"en": "network anomaly", "pt_BR": "Anomalia de rede", "level": "error"},
        206: {"en": "AI detected print quality issues", "pt_BR": "IA detectou problema de qualidade na impressão", "level": "error"},
        207: {"en": "z-Touch abnormality", "pt_BR": "Anormalidade no z-Touch", "level": "error"},
        208: {"en": "File anomaly", "pt_BR": "Arquivo anormal", "level": "error"},
        209: {"en": "Abnormal hot bed temperature", "pt_BR": "Temperatura anormal da mesa aquecida", "level": "error"},
        500: {"en": "Printing quality issues detected by AI", "pt_BR": "IA detectou problema de qualidade na impressão", "level": "error", "cc": "AC0500"},
        501: {"en": "TF0501: Heat break fan speed detected to be too low.", "pt_BR": "TF0501: rotação do ventilador do heat break detectada como muito baixa", "level": "error", "cc": "TF0501"},
        502: {"en": "Mainboard fan exception", "pt_BR": "Falha no ventilador da placa-mãe", "level": "error", "cc": "CF0502"},
        503: {"en": "Detected a issue with the print quality of the first layer", "pt_BR": "Problema de qualidade detectado na primeira camada", "level": "error", "cc": "AC0503"},
        504: {"en": "Current file does not support flow detection", "pt_BR": "O arquivo atual não suporta a detecção de fluxo", "level": "error", "cc": "AL0504"},
        505: {"en": "AI LiDAR anomaly", "pt_BR": "Anomalia no LiDAR de IA", "level": "error", "cc": "AL0505"},
        506: {"en": "AI LiDAR calibration failure", "pt_BR": "Falha na calibração do LiDAR de IA", "level": "error", "cc": "AL0506"},
        507: {"en": "AI LiDAR alignment failure", "pt_BR": "Falha no alinhamento do LiDAR de IA", "level": "error", "cc": "AL0507"},
        508: {"en": "Current storage space usage has reached {0}, please clean up in time", "pt_BR": "O uso do armazenamento chegou a {0}; faça uma limpeza em breve", "level": "error", "cc": "BS0508"},
        509: {"en": "Chamber temp too high, camera off. It'll auto-on when temp normalizes", "pt_BR": "Temperatura da câmara muito alta; câmera desligada. Ela religa sozinha quando a temperatura normalizar", "level": "error", "cc": "AC0509"},
        510: {"en": "Camera failed to start, try restarting", "pt_BR": "Falha ao iniciar a câmera; tente reiniciar", "level": "error", "cc": "AC0510"},
        511: {"en": "Current file does not support flow ratio calibration, printing with default values.", "pt_BR": "O arquivo atual não suporta a calibração da taxa de fluxo; imprimindo com os valores padrão", "level": "error", "cc": "AC0511"},
        512: {"en": "Current filament does not support flow ratio calibration, printing with default values.", "pt_BR": "O filamento atual não suporta a calibração da taxa de fluxo; imprimindo com os valores padrão", "level": "error", "cc": "AC0512"},
        513: {"en": "Auto PA calibration image anomaly, printing with default values.", "pt_BR": "Imagem anormal na calibração automática de PA; imprimindo com os valores padrão", "level": "error", "cc": "AC0513"},
        514: {"en": "Auto PA calibration failed, printing with default values.", "pt_BR": "Falha na calibração automática de PA; imprimindo com os valores padrão", "level": "error", "cc": "AC0514"},
        515: {"en": "Flow ratio calibration image anomaly, printing with default values.", "pt_BR": "Imagem anormal na calibração da taxa de fluxo; imprimindo com os valores padrão", "level": "error", "cc": "AC0515"},
        516: {"en": "Flow ratio calibration failed, printing with default values.", "pt_BR": "Falha na calibração da taxa de fluxo; imprimindo com os valores padrão", "level": "error", "cc": "AC0516"},
        523: {"en": "A model warping issue has been detected.", "pt_BR": "Empenamento (warping) do modelo detectado", "level": "error", "cc": "AC0523"},
        526: {"en": "TF0526: Model fan speed detected to be too low.", "pt_BR": "TF0526: rotação do ventilador do modelo detectada como muito baixa", "level": "error", "cc": "TF0526"},
        527: {"en": "Detected waste chute is block.", "pt_BR": "Calha de descarte entupida", "level": "error", "cc": "AC0527"},
        528: {"en": "FO0528: rinter seems to be printing without extruding.", "pt_BR": "FO0528: a impressora parece estar imprimindo sem extrudar", "level": "error", "cc": "FO0528"},
        529: {"en": "Nozzle camera failed to start, try restarting.", "pt_BR": "Falha ao iniciar a câmera do bico; tente reiniciar", "level": "error", "cc": "AC0529"},
        800: {"en": "Axis movement error", "pt_BR": "Erro de movimento dos eixos", "level": "error"},
        801: {"en": "Machine preparation", "pt_BR": "Preparando a máquina", "level": "error"},
        2000: {"en": "System error, try restarting", "pt_BR": "Erro de sistema; tente reiniciar", "level": "warning", "cc": "XS2000"},
        2001: {"en": "System error, try restarting", "pt_BR": "Erro de sistema; tente reiniciar", "level": "warning", "cc": "XS2001"},
        2022: {"en": "An unknown error was detected in printing", "pt_BR": "Erro desconhecido detectado durante a impressão", "level": "error"},
        2060: {"en": "System error, try restarting", "pt_BR": "Erro de sistema; tente reiniciar", "level": "warning", "cc": "XS2060"},
        2093: {"en": "System error, try restarting", "pt_BR": "Erro de sistema; tente reiniciar", "level": "warning", "cc": "XS2093"},
        2111: {"en": "The nozzle temperature is below the minimum extrusion temperature", "pt_BR": "A temperatura do bico está abaixo do mínimo para extrusão", "level": "warning", "cc": "TE2111"},
        2243: {"en": "Axis movement exceeded limit", "pt_BR": "Movimento do eixo ultrapassou o limite", "level": "error", "cc": "CZ2243"},
        2352: {"en": "Hotbed failed to return to the maximum Z-axis position or tilt calibration at this position failed,please try again after resolving.", "pt_BR": "A mesa não voltou à posição máxima do eixo Z ou a calibração de inclinação falhou nessa posição; resolva e tente novamente", "level": "error", "cc": "CZ2863"},
        2355: {"en": "MCU task scheduling failed, possibly due to high load or communication issues.", "pt_BR": "Falha no agendamento de tarefas da MCU, possivelmente por carga alta ou problema de comunicação", "level": "error", "cc": "BM2355"},
        2357: {"en": "Hotbed failed to reach the maximum position. Check for sensor issues or bed tilt.Please check and retry.", "pt_BR": "A mesa não alcançou a posição máxima. Verifique problemas no sensor ou inclinação da mesa e tente novamente", "level": "error", "cc": "CL2357"},
        2509: {"en": "Unable to obtain nozzle temperature, the thermistor may be disconnected", "pt_BR": "Não foi possível ler a temperatura do bico; o termistor pode estar desconectado", "level": "warning", "cc": "TE2509"},
        2510: {"en": "Hotbed thermistor open circuit", "pt_BR": "Termistor da mesa aquecida em circuito aberto", "level": "warning", "cc": "CB2510"},
        2511: {"en": "The chamber thermistor may be open circuit", "pt_BR": "O termistor da câmara pode estar em circuito aberto", "level": "error", "cc": "CT2511"},
        2512: {"en": "The motherboard temperature is abnormal", "pt_BR": "Temperatura anormal da placa-mãe", "level": "warning", "cc": "BM2512"},
        2515: {"en": "Nozzle temperature exceeds limit, thermistor may be short-circuited", "pt_BR": "Temperatura do bico acima do limite; o termistor pode estar em curto-circuito", "level": "warning", "cc": "TE2515"},
        2516: {"en": "Hotbed thermistor short circuit", "pt_BR": "Termistor da mesa aquecida em curto-circuito", "level": "warning", "cc": "CB2516"},
        2517: {"en": "The chamber thermistor may be short circuit", "pt_BR": "O termistor da câmara pode estar em curto-circuito", "level": "error", "cc": "CT2517"},
        2519: {"en": "Chamber heater fan abnormal, chamber heating closed.", "pt_BR": "Ventilador do aquecedor da câmara anormal; aquecimento da câmara desligado", "level": "error", "cc": "CP2519"},
        2559: {"en": "Chamber heating issue, please ensure the printer is sealed", "pt_BR": "Problema no aquecimento da câmara; verifique se a impressora está bem vedada", "level": "error", "cc": "CP2559"},
        2564: {"en": "The nozzle is not heating as expected", "pt_BR": "O bico não está aquecendo como esperado", "level": "warning", "cc": "TE2564"},
        2565: {"en": "The heat bed is not heating as expected", "pt_BR": "A mesa aquecida não está aquecendo como esperado", "level": "warning", "cc": "CB2565"},
        2571: {"en": "Print file is corrupted", "pt_BR": "Arquivo de impressão corrompido", "level": "warning", "cc": "XD2571"},
        2573: {"en": "X-axis homing abnormality", "pt_BR": "Anormalidade no homing do eixo X", "level": "warning", "cc": "CX2573"},
        2577: {"en": "Y-axis homing abnormality", "pt_BR": "Anormalidade no homing do eixo Y", "level": "warning", "cc": "CY2577"},
        2581: {"en": "Z-axis homing abnormality", "pt_BR": "Anormalidade no homing do eixo Z", "level": "warning", "cc": "CZ2581"},
        2585: {"en": "X-axis print coordinates out of range", "pt_BR": "Coordenadas de impressão do eixo X fora do limite", "level": "warning", "cc": "CX2585"},
        2586: {"en": "Y-axis print coordinates out of range", "pt_BR": "Coordenadas de impressão do eixo Y fora do limite", "level": "warning", "cc": "CY2586"},
        2587: {"en": "Z-axis print coordinates out of range", "pt_BR": "Coordenadas de impressão do eixo Z fora do limite", "level": "warning", "cc": "CZ2587"},
        2588: {"en": "Z Offset Exceeds Limit", "pt_BR": "Z Offset fora do limite", "level": "warning", "cc": "CZ2588"},
        2710: {"en": "Belt tension module abnormal.", "pt_BR": "Módulo de tensionamento da correia anormal", "level": "error", "cc": "CA2710"},
        2711: {"en": "Auto belt tension failure", "pt_BR": "Falha no tensionamento automático da correia", "level": "error", "cc": "CA2711"},
        2712: {"en": "Chamber heater fan abnormal, chamber heating closed.", "pt_BR": "Ventilador do aquecedor da câmara anormal; aquecimento da câmara desligado", "level": "error", "cc": "CA2712"},
        2713: {"en": "Belt tension module error, left side belt communication line may be connected improperly.", "pt_BR": "Erro no módulo de tensionamento: o cabo de comunicação da correia esquerda pode estar mal conectado", "level": "error", "cc": "CA2713"},
        2714: {"en": "Belt tension module error, right side belt tension module not calibrated, please recalibrate the module.", "pt_BR": "Erro no módulo de tensionamento: módulo da correia direita não calibrado; recalibre o módulo", "level": "error", "cc": "CA2714"},
        2715: {"en": "Belt tension module error, left side belt tension module not calibrated, please recalibrate the module.", "pt_BR": "Erro no módulo de tensionamento: módulo da correia esquerda não calibrado; recalibre o módulo", "level": "error", "cc": "CA2715"},
        2716: {"en": "Belt tension module error, please troubleshoot and try recalibrating.", "pt_BR": "Erro no módulo de tensionamento da correia; diagnostique e tente recalibrar", "level": "error", "cc": "CA2716"},
        2717: {"en": "Belt tension module error, please troubleshoot and try recalibrating.", "pt_BR": "Erro no módulo de tensionamento da correia; diagnostique e tente recalibrar", "level": "error", "cc": "CA2717"},
        2718: {"en": "Belt tension timeout failure, please try retightening or restarting.", "pt_BR": "Tempo esgotado no tensionamento da correia; tente tensionar de novo ou reinicie", "level": "error", "cc": "CA2718"},
        2719: {"en": "Belt tension timeout failure, please try retightening or restarting.", "pt_BR": "Tempo esgotado no tensionamento da correia; tente tensionar de novo ou reinicie", "level": "error", "cc": "CA2719"},
        2720: {"en": "Abnormal belt tensioning module, right belt tensioning module calibration value abnormal, please recalibrate the module.", "pt_BR": "Módulo de tensionamento anormal: valor de calibração da correia direita anormal; recalibre o módulo", "level": "error", "cc": "CA2720"},
        2721: {"en": "Abnormal belt tensioning module, left belt tensioning module calibration value abnormal, please recalibrate the module.", "pt_BR": "Módulo de tensionamento anormal: valor de calibração da correia esquerda anormal; recalibre o módulo", "level": "error", "cc": "CA2721"},
        2722: {"en": "Detection of a low resonance frequency on the X-axis may be due to a loose belt.Continuing the print could affect quality. It's recommended to cancel,check belt tension, and recalibrate.", "pt_BR": "Baixa frequência de ressonância detectada no eixo X, possivelmente por correia frouxa. Continuar pode afetar a qualidade. Recomenda-se cancelar, verificar a tensão da correia e recalibrar", "level": "warning", "cc": "CA2722"},
        2723: {"en": "Detection of a low resonance frequency on the Y-axis may be due to a loose belt.Continuing the print could affect quality. It's recommended to cancel,check belt tension, and recalibrate.", "pt_BR": "Baixa frequência de ressonância detectada no eixo Y, possivelmente por correia frouxa. Continuar pode afetar a qualidade. Recomenda-se cancelar, verificar a tensão da correia e recalibrar", "level": "warning", "cc": "CA2723"},
        2761: {"en": "Unable to read nozzle leveling sensor data, the connection between the nozzle and the motherboard may be loose or disconnected.", "pt_BR": "Não foi possível ler o sensor de nivelamento do bico; a conexão entre o bico e a placa-mãe pode estar solta ou desconectada", "level": "warning", "cc": "TE2761"},
        2762: {"en": "The nozzle leveling sensor may be damaged or the wiping module may have fallen off. Please check or try restarting.", "pt_BR": "O sensor de nivelamento do bico pode estar danificado ou o módulo de limpeza pode ter caído. Verifique ou tente reiniciar", "level": "warning", "cc": "TE2762"},
        2768: {"en": "Z-axis homing issue,may have an external disturbance", "pt_BR": "Problema no homing do eixo Z; pode haver interferência externa", "level": "warning", "cc": "CZ2768"},
        2781: {"en": "[Printing] XY Motor: XY motor speed limit exceeded. The motor will be released. Please reduce the print speed or re-slice and print again.Z Motor: Z motor speed limit exceeded. The motor will be released. Please reduce the print speed or re-slice and print again.E Motor: Extruder speed limit exceeded. The motor will be released. Please reduce the print speed or re-slice and print again.[Not Printing] XY Motor: XY motor speed limit exceeded. The motor will be released. Please reduce the XY motor movement speed.Z Motor: Z motor speed limit exceeded. The motor will be released. Please reduce the Z motor movement speed.E Motor: Extruder speed limit exceeded. The motor will be released. Please reduce the extruder movement speed.", "pt_BR": "[Imprimindo] Motor XY: limite de velocidade excedido; o motor será liberado. Reduza a velocidade ou refatie e imprima de novo. Motor Z: limite de velocidade excedido; o motor será liberado. Reduza a velocidade ou refatie e imprima de novo. Motor E: limite de velocidade do extrusor excedido; o motor será liberado. Reduza a velocidade ou refatie e imprima de novo. [Parada] Motor XY/Z/E: limite de velocidade excedido; o motor será liberado. Reduza a velocidade do movimento", "level": "warning", "cc": "CM2781"},
        2782: {"en": "[X/Y/E/Z1/Z2] step-servo position issue, please try again after resolving", "pt_BR": "[X/Y/E/Z1/Z2] problema de posição do step-servo; resolva e tente novamente", "level": "warning", "cc": "CM2782"},
        2783: {"en": "[Printing] XY Motor: An obstacle was detected during the extruder's movement. This print has been canceled and the motor is released.Z Motor: An obstacle was detected during the heated bed's movement. This print has been canceled and the motor is released.E Motor: An extrusion anomaly was detected, possibly a clog, tangle, or jam. Printing has been paused.[Not Printing] XY Motorstep-servo: An obstacle was detected during the nozzle's movement. The motor will be released.Z Motor: An obstacle was detected during the heated bed's movement. The motor will be released.E Motor: An extrusion anomaly was detected, possibly a clog, tangle, or jam. The motor will be released.", "pt_BR": "[Imprimindo] Motor XY: obstáculo detectado no movimento do extrusor; impressão cancelada e motor liberado. Motor Z: obstáculo detectado no movimento da mesa; impressão cancelada e motor liberado. Motor E: anomalia de extrusão detectada (possível entupimento, emaranhado ou travamento); impressão pausada. [Parada] Obstáculo no movimento do bico ou da mesa, ou anomalia de extrusão; o motor será liberado", "level": "warning", "cc": "CM2783"},
        2784: {"en": "Abnormal resistance in the Y-axis movement direction, motor released. Fix issue and restart.", "pt_BR": "Resistência anormal no movimento do eixo Y; motor liberado. Resolva e reinicie", "level": "error", "cc": "CM2784"},
        2785: {"en": "Abnormal resistance under the hotbed, motor released. Fix issue and restart.", "pt_BR": "Resistência anormal sob a mesa aquecida; motor liberado. Resolva e reinicie", "level": "error", "cc": "CM2785"},
        2786: {"en": "Extruder speed limit exceeded. The motor will be released. Please reduce the extruder movement speed.", "pt_BR": "Limite de velocidade do extrusor excedido; o motor será liberado. Reduza a velocidade de movimento do extrusor", "level": "error", "cc": "CM2786"},
        2788: {"en": "[x or y or e] axis servo motor hardware abnormality/[z1 or z2] axis servo motor hardware abnormality, the platform plate will drop", "pt_BR": "Anormalidade de hardware no servo-motor do eixo [x, y ou e] / [z1 ou z2]; a plataforma vai descer", "level": "error", "cc": "CM2788"},
        2789: {"en": "Extruder extrusion anomaly, possible clog or blockage. motor released.", "pt_BR": "Anomalia de extrusão, possível entupimento ou bloqueio; motor liberado", "level": "error", "cc": "CM2789"},
        2790: {"en": "X Motor: X motor identification error detected. The motor wire may be connected improperly or the motor may be damaged.Y Motor: Y motor identification error detected. The motor wire may be connected improperly or the motor may be damaged.Z1 Motor: Z1 motor identification error detected. The motor wire may be connected improperly or the motor may be damaged.Z2 Motor: Z2 motor identification error detected. The motor wire may be connected improperly or the motor may be damaged.E Motor: Extruder motor identification error detected. The motor wire may be connected improperly or the motor may be damaged.", "pt_BR": "Erro de identificação dos motores X, Y, Z1, Z2 ou E (extrusor): o cabo do motor pode estar mal conectado ou o motor danificado", "level": "warning", "cc": "CM2790"},
        2791: {"en": "Y motor identification error detected. The motor wire may be connected improperly or the motor may be damaged.", "pt_BR": "Erro de identificação do motor Y: o cabo do motor pode estar mal conectado ou o motor danificado", "level": "error", "cc": "CM2791"},
        2792: {"en": "Z1 motor identification error detected. The motor wire may be connected improperly or the motor may be damaged.", "pt_BR": "Erro de identificação do motor Z1: o cabo do motor pode estar mal conectado ou o motor danificado", "level": "error", "cc": "CM2792"},
        2793: {"en": "Z2 motor identification error detected. The motor wire may be connected improperly or the motor may be damaged.", "pt_BR": "Erro de identificação do motor Z2: o cabo do motor pode estar mal conectado ou o motor danificado", "level": "error", "cc": "CM2791"},
        2794: {"en": "E motor identification error detected. The motor wire may be connected improperly or the motor may be damaged.", "pt_BR": "Erro de identificação do motor E (extrusor): o cabo do motor pode estar mal conectado ou o motor danificado", "level": "error", "cc": "CM2794"},
        2797: {"en": "Extruder MCU temperature is too high, possibly due to high chamber temperature or delayed cooling of the extruder motherboard.", "pt_BR": "Temperatura da MCU do extrusor muito alta, possivelmente por câmara quente ou resfriamento lento da placa do extrusor", "level": "error", "cc": "BM2797"},
        2798: {"en": "X/Y/Z/E axis step-servo motor initialization error, motor communication line may be connected incorrectly", "pt_BR": "Erro de inicialização dos step-servo X/Y/Z/E; o cabo de comunicação do motor pode estar ligado errado", "level": "warning", "cc": "CM2798"},
        2799: {"en": "X/Y/Z/E axis step-servo motor initialization error, motor communication line may be connected incorrectly", "pt_BR": "Erro de inicialização dos step-servo X/Y/Z/E; o cabo de comunicação do motor pode estar ligado errado", "level": "error", "cc": "CM2799"},
        2800: {"en": "X/Y/Z/E axis step-servo motor initialization error, motor communication line may be connected incorrectly", "pt_BR": "Erro de inicialização dos step-servo X/Y/Z/E; o cabo de comunicação do motor pode estar ligado errado", "level": "error", "cc": "CM2800"},
        2801: {"en": "X/Y/Z/E axis step-servo motor initialization error, motor communication line may be connected incorrectly", "pt_BR": "Erro de inicialização dos step-servo X/Y/Z/E; o cabo de comunicação do motor pode estar ligado errado", "level": "error", "cc": "CM2799"},
        2802: {"en": "X/Y/Z/E axis step-servo motor initialization error, motor communication line may be connected incorrectly", "pt_BR": "Erro de inicialização dos step-servo X/Y/Z/E; o cabo de comunicação do motor pode estar ligado errado", "level": "error", "cc": "CM2802"},
        2831: {"en": "communication issue, please try again after resolving.", "pt_BR": "Problema de comunicação; resolva e tente novamente", "level": "warning", "cc": "FS2831", "cfs": 1},
        2832: {"en": "Retract issue, check for filament or spool jams. Click \"Retry\" after resolving.", "pt_BR": "Problema na retração; verifique se o filamento ou o carretel está travado e clique em \"Tentar novamente\"", "level": "warning", "cc": "FR2832", "cfs": 1},
        2833: {"en": "Feed issue, check for filament or spool jams. Click \"Retry\" after resolving.", "pt_BR": "Problema na alimentação; verifique se o filamento ou o carretel está travado e clique em \"Tentar novamente\"", "level": "warning", "cc": "FR2833", "cfs": 1},
        2834: {"en": "System error.", "pt_BR": "Erro de sistema", "level": "error", "cc": "FS2834", "cfs": 1},
        2835: {"en": "Feed issue , filament may be stuck from loader to CFS hub. Please resolve the issue and click the \"Retry\" button.", "pt_BR": "Problema na alimentação: o filamento pode estar preso entre o carregador e o hub do CFS. Resolva e clique em \"Tentar novamente\"", "level": "warning", "cc": "FR2835", "cfs": 1},
        2836: {"en": "Feed issue , filament may be blocked between the CFS hub and extruder filament detector. Please resolve the issue and click \"Retry\" button.", "pt_BR": "Problema na alimentação: o filamento pode estar bloqueado entre o hub do CFS e o detector de filamento do extrusor. Resolva e clique em \"Tentar novamente\"", "level": "warning", "cc": "FR2836", "cfs": 1},
        2837: {"en": "Feed issue , filament may be jammed between the extruder sensor and the extruder gear. Please resolve the issue and click the \"Retry\" button.", "pt_BR": "Problema na alimentação: o filamento pode estar travado entre o sensor do extrusor e a engrenagem. Resolva e clique em \"Tentar novamente\"", "level": "warning", "cc": "FO2837", "cfs": 1},
        2838: {"en": "Feed issue , filament may be stuck between the CFS hub sensor and the gear. Please resolve the issue and click the \"Retry\" button.", "pt_BR": "Problema na alimentação: o filamento pode estar preso entre o sensor do hub do CFS e a engrenagem. Resolva e clique em \"Tentar novamente\"", "level": "warning", "cc": "FO2838", "cfs": 1},
        2839: {"en": "filament runs out, please refill.", "pt_BR": "Filamento acabou; recarregue", "level": "warning", "cc": "FR2839", "cfs": 1},
        2840: {"en": "Command failed, current scenario does not support this command.", "pt_BR": "Comando falhou; o cenário atual não suporta este comando", "level": "error", "cc": "FS2840", "cfs": 1},
        2841: {"en": "Cutter issue, cutter might be jammed, please try again after resolving", "pt_BR": "Problema no cortador; ele pode estar travado. Resolva e tente novamente", "level": "warning", "cc": "TC2841"},
        2843: {"en": "RFID cannot be read, please manually edit filament", "pt_BR": "Não foi possível ler o RFID; edite o filamento manualmente", "level": "error", "cc": "FS2843", "cfs": 1},
        2844: {"en": "PTFE tube may have detached from the pneumatic fitting, please try again after resolving.", "pt_BR": "O tubo PTFE pode ter se soltado da conexão pneumática; resolva e tente novamente", "level": "warning", "cc": "FB2844", "cfs": 1},
        2845: {"en": "Extruder may be clogged. Click \"Retry\" after resolving.", "pt_BR": "O extrusor pode estar entupido. Clique em \"Tentar novamente\" após resolver", "level": "warning", "cc": "FO2845", "cfs": 1},
        2846: {"en": "The filament buffer signal is abnormal,the buffer may be stuck or the filament may be tangle.", "pt_BR": "Sinal anormal do buffer de filamento; o buffer pode estar travado ou o filamento emaranhado", "level": "warning", "cc": "FB2846", "cfs": 1},
        2847: {"en": "Filament may tangled, please try again after resolving", "pt_BR": "O filamento pode estar emaranhado; resolva e tente novamente", "level": "warning", "cc": "FB2847", "cfs": 1},
        2848: {"en": "filament may be broken in CFS.", "pt_BR": "O filamento pode ter quebrado no CFS", "level": "warning", "cc": "FR2848", "cfs": 1},
        2849: {"en": "Retract issue, filament cannot be returned to CFS hub. Please resolve the issue and click the \"Retry\" button.", "pt_BR": "Problema na retração: o filamento não consegue voltar ao hub do CFS. Resolva e clique em \"Tentar novamente\"", "level": "warning", "cc": "FR2849", "cfs": 1},
        2850: {"en": "Retract issue, filaments may be inserted into the CFS hub. Remove and click the \"Retry\" button.", "pt_BR": "Problema na retração: filamentos podem ter entrado no hub do CFS. Remova e clique em \"Tentar novamente\"", "level": "warning", "cc": "FR2850", "cfs": 1},
        2851: {"en": "Retract issue, Filament Buffer failure. Please resolve and click \"Retry\" button.", "pt_BR": "Problema na retração: falha no buffer de filamento. Resolva e clique em \"Tentar novamente\"", "level": "warning", "cc": "FR2851", "cfs": 1},
        2852: {"en": "TR2852: Filament detector triggered abnormally, possibly due to an external material rack being used with CFS.", "pt_BR": "TR2852: detector de filamento acionado de forma anormal, possivelmente por suporte de carretel externo usado junto com o CFS", "level": "error", "cc": "TR2852"},
        2853: {"en": "CFS temperature and humidity sensor abnormal.", "pt_BR": "Sensor de temperatura e umidade do CFS anormal", "level": "error", "cc": "FH2853"},
        2854: {"en": "TC2854: Filament detected in the extruder, affecting the cutter calibration. Please remove the filament and try again.", "pt_BR": "TC2854: filamento detectado no extrusor, atrapalhando a calibração do cortador. Remova o filamento e tente novamente", "level": "warning", "cc": "TC2854"},
        2855: {"en": "TC2855: Cutter calibrat failed. Please try again after resolving.", "pt_BR": "TC2855: falha na calibração do cortador. Resolva e tente novamente", "level": "warning", "cc": "TC2855"},
        2856: {"en": "TC2856: Cutter issue, the cutter may be jammed or missing. Click \"Retry\" after resolving.", "pt_BR": "TC2856: problema no cortador; ele pode estar travado ou ausente. Clique em \"Tentar novamente\" após resolver", "level": "warning", "cc": "TC2856"},
        2857: {"en": "CFS feeding motor overload, maybe excessive load or unload resistance or the filament tangled or stuck.", "pt_BR": "Sobrecarga do motor de alimentação do CFS: carga excessiva ou resistência ao carregar/descarregar, ou filamento emaranhado/travado", "level": "warning", "cc": "FM2857"},
        2858: {"en": "FS2858: CFS EEPROM abnormal.", "pt_BR": "FS2858: EEPROM do CFS anormal", "level": "error", "cc": "FS2858"},
        2859: {"en": "FO2859: CFS filament odometry abnormal.", "pt_BR": "FO2859: odometria de filamento do CFS anormal", "level": "error", "cc": "FO2859"},
        2860: {"en": "FB2860: CFS filament buffer abnormal.", "pt_BR": "FB2860: buffer de filamento do CFS anormal", "level": "error", "cc": "FB2860"},
        2861: {"en": "FS2861: CFS left RFID board abnormal.", "pt_BR": "FS2861: placa RFID esquerda do CFS anormal", "level": "error", "cc": "FS2861"},
        2862: {"en": "FS2862: CFS right RFID board abnormal.", "pt_BR": "FS2862: placa RFID direita do CFS anormal", "level": "error", "cc": "FS2862"},
        2863: {"en": "TR2858:Retract issue, extruder filament sensor anomaly triggered, filament may have broken inside the extruder.", "pt_BR": "TR2858: problema na retração; sensor de filamento do extrusor acionado de forma anormal, o filamento pode ter quebrado dentro do extrusor", "level": "error", "cc": "TR2863"},
        2864: {"en": "Feed issue, filament buffer failure. Please resolve and click \"Retry\" button.", "pt_BR": "Problema na alimentação: falha no buffer de filamento. Resolva e clique em \"Tentar novamente\"", "level": "error", "cc": "FB2864"},
        2865: {"en": "Feed issue, filament failed to exit extruder.Please resolve and click 'Retry' button.", "pt_BR": "Problema na alimentação: o filamento não saiu do extrusor. Resolva e clique em \"Tentar novamente\"", "level": "error", "cc": "FR2865"},
        2866: {"en": "Cutter not detected.", "pt_BR": "Cortador não detectado", "level": "error", "cc": "TC2866"},
        2870: {"en": "CFS2 {0} {1} chamber heating abnormality detected. {2} drying has stopped! Please check the device status or environment and try again", "pt_BR": "CFS2 {0}, câmara {1}: aquecimento anormal detectado. A secagem ({2}) foi interrompida! Verifique o equipamento ou o ambiente e tente novamente", "level": "warning", "cc": "FT2870"},
        2871: {"en": "CFS2 {0} {1} chamber heating temperature exceeded the upper limit. {2} drying has stopped! Please check the device and try again", "pt_BR": "CFS2 {0}, câmara {1}: temperatura de aquecimento acima do limite. A secagem ({2}) foi interrompida! Verifique o equipamento e tente novamente", "level": "warning", "cc": "FT2871"},
        2872: {"en": "CFS2 {0} {1} chamber heating component abnormality detected. {2} drying has stopped! Please check the device and try again", "pt_BR": "CFS2 {0}, câmara {1}: componente de aquecimento anormal. A secagem ({2}) foi interrompida! Verifique o equipamento e tente novamente", "level": "warning", "cc": "FP2872"},
        2873: {"en": "CFS2 {0} {1} chamber temperature sensor failure detected. {2} drying has stopped! Please check the device and try again", "pt_BR": "CFS2 {0}, câmara {1}: falha no sensor de temperatura. A secagem ({2}) foi interrompida! Verifique o equipamento e tente novamente", "level": "warning", "cc": "FT2873"},
        2874: {"en": "CFS2 {0} {1} chamber fan failure detected. {2} drying has stopped! Please check the device and try again", "pt_BR": "CFS2 {0}, câmara {1}: falha no ventilador. A secagem ({2}) foi interrompida! Verifique o equipamento e tente novamente", "level": "warning", "cc": "FF2874"},
        2875: {"en": "Independent drying power supply disconnected detected. Please check the power status and try again!", "pt_BR": "Fonte de alimentação independente de secagem desconectada. Verifique a alimentação e tente novamente!", "level": "warning", "cc": "FV2875"},
        2931: {"en": "CFSLite communication issue, try again.", "pt_BR": "Problema de comunicação com o CFS Lite; tente novamente", "level": "warning", "cc": "FS2931"},
        2934: {"en": "Instruction data error, transmission failed.", "pt_BR": "Erro nos dados da instrução; falha na transmissão", "level": "error", "cc": "FS2934"},
        2936: {"en": "Load error. Extruder filament sensor not triggered; filament not loaded into extruder.", "pt_BR": "Erro ao carregar: o sensor de filamento do extrusor não foi acionado; o filamento não entrou no extrusor", "level": "warning", "cc": "FO2936"},
        2937: {"en": "Load error. Filament odometry rotation below expected; filament may be jammed between sensor and drive gear.", "pt_BR": "Erro ao carregar: rotação da odometria abaixo do esperado; o filamento pode estar travado entre o sensor e a engrenagem", "level": "warning", "cc": "FO2937"},
        2939: {"en": "No.{0} filament runs out, please refill.", "pt_BR": "O filamento nº {0} acabou; recarregue", "level": "warning", "cc": "FR2939"},
        2940: {"en": "Command failed, current scenario does not support this command.", "pt_BR": "Comando falhou; o cenário atual não suporta este comando", "level": "error", "cc": "FS2940"},
        2943: {"en": "No.1 RFID cannot be read, please manually edit filament", "pt_BR": "Não foi possível ler o RFID nº 1; edite o filamento manualmente", "level": "error", "cc": "FS2943"},
        2944: {"en": "Load error. Filament odometry normal, but extruder filament sensor not triggered for long; PTFE tube may be detached.", "pt_BR": "Erro ao carregar: odometria normal, mas o sensor do extrusor não acionou por muito tempo; o tubo PTFE pode ter se soltado", "level": "warning", "cc": "FB2944"},
        2945: {"en": "Filament odometry rotation below expected; air printing detected; possible extruder clog.", "pt_BR": "Rotação da odometria abaixo do esperado; impressão no ar detectada; possível entupimento do extrusor", "level": "warning", "cc": "FO2945"},
        2947: {"en": "Buffer stays at empty limit; filament may be tangled.", "pt_BR": "O buffer permanece no limite vazio; o filamento pode estar emaranhado", "level": "warning", "cc": "FB2947"},
        2949: {"en": "Unload error. Filament cannot be returned to CFS hub. Please resolve and retry.", "pt_BR": "Erro ao descarregar: o filamento não consegue voltar ao hub do CFS. Resolva e tente novamente", "level": "warning", "cc": "FR2949"},
        2952: {"en": "TR2952:Filament detector triggered abnormally, filament may have broken inside the extruder or inserted into a spool holder filament.", "pt_BR": "TR2952: detector de filamento acionado de forma anormal; o filamento pode ter quebrado no extrusor ou ter sido inserido num suporte de carretel", "level": "error", "cc": "TR2952"},
        2953: {"en": "CFSLite temperature and humidity sensor abnormal.", "pt_BR": "Sensor de temperatura e umidade do CFS Lite anormal", "level": "error", "cc": "FH2953"},
        2958: {"en": "CFSLite EEPROM abnormal.", "pt_BR": "EEPROM do CFS Lite anormal", "level": "error", "cc": "FS2958"},
        2961: {"en": "CFSLite left RFID board abnormal.", "pt_BR": "Placa RFID esquerda do CFS Lite anormal", "level": "error", "cc": "FS2961"},
        2962: {"en": "CFSLite right RFID board abnormal.", "pt_BR": "Placa RFID direita do CFS Lite anormal", "level": "error", "cc": "FS2962"},
        2963: {"en": "Retract issue, extruder filament sensor anomaly triggered, filament may have broken inside the extruder.", "pt_BR": "Problema na retração: sensor de filamento do extrusor acionado de forma anormal; o filamento pode ter quebrado dentro do extrusor", "level": "warning", "cc": "TR2963"},
        2964: {"en": "Feed issue , the buffer cannot reach full limit,possible planing. Please resolve the issue and click the \"Retry\" button.", "pt_BR": "Problema na alimentação: o buffer não chega ao limite cheio, possível raspagem do filamento. Resolva e clique em \"Tentar novamente\"", "level": "warning", "cc": "FB2964"},
        2970: {"en": "Unload error. Load/unload motor stalled; filament cannot exit extruder.", "pt_BR": "Erro ao descarregar: o motor de carga/descarga travou; o filamento não sai do extrusor", "level": "warning", "cc": "FO2970"},
        2971: {"en": "Preloading error; delivery overdue. Please resolve and retry.", "pt_BR": "Erro no pré-carregamento; entrega atrasada. Resolva e tente novamente", "level": "warning", "cc": "FO2971"},
        3000: {"en": "System error, try restarting", "pt_BR": "Erro de sistema; tente reiniciar", "level": "warning", "cc": "XS3000"},
        3001: {"en": "System error, try restarting", "pt_BR": "Erro de sistema; tente reiniciar", "level": "warning", "cc": "XS3001"},
        3002: {"en": "System error, try restarting", "pt_BR": "Erro de sistema; tente reiniciar", "level": "warning", "cc": "XS3002"},
        3101: {"en": "Instruction data error, transmission failed.", "pt_BR": "Erro nos dados da instrução; falha na transmissão", "level": "error", "cc": "FS3101"},
        3103: {"en": "Command failed, current scenario does not support this command.", "pt_BR": "Comando falhou; o cenário atual não suporta este comando", "level": "error", "cc": "FS3103"},
        3119: {"en": "Load error. Filament sensor not triggered, filament not loaded into extruder.", "pt_BR": "Erro ao carregar: sensor de filamento não acionado; o filamento não entrou no extrusor", "level": "warning", "cc": "FB3119"},
        3120: {"en": "Load error. Buffer not at full limit; PTFE tube may be detached or filament grinding detected.", "pt_BR": "Erro ao carregar: o buffer não está no limite cheio; o tubo PTFE pode ter se soltado ou o filamento está sendo raspado", "level": "warning", "cc": "FO3120"},
        3121: {"en": "Load error. Buffer not at empty limit, filament not gripped or clog detected.", "pt_BR": "Erro ao carregar: o buffer não está no limite vazio; o filamento não foi tracionado ou há entupimento", "level": "warning", "cc": "FO3121"},
        3134: {"en": "Unload error. Buffer sensor did not trigger; possible filament grinding.", "pt_BR": "Erro ao descarregar: o sensor do buffer não acionou; possível raspagem do filamento", "level": "warning", "cc": "FO3134"},
        3135: {"en": "Unload error. Filament sensor remains triggered, filament not unloaded.", "pt_BR": "Erro ao descarregar: o sensor de filamento continua acionado; o filamento não foi descarregado", "level": "warning", "cc": "FR3135"},
        3149: {"en": "Filament runs out, please refill.", "pt_BR": "Filamento acabou; recarregue", "level": "warning", "cc": "FR3149"},
        3151: {"en": "Buffer stays at empty limit, possible filament tangling.", "pt_BR": "O buffer permanece no limite vazio; possível emaranhado do filamento", "level": "warning", "cc": "FB3151"},
        3152: {"en": "TR3152:Filament detector triggered abnormally, filament may have broken inside the extruder or inserted into a spool holder filament.", "pt_BR": "TR3152: detector de filamento acionado de forma anormal; o filamento pode ter quebrado no extrusor ou ter sido inserido num suporte de carretel", "level": "error", "cc": "TR3152"},
        3160: {"en": "CFSmini communication issue, try again.", "pt_BR": "Problema de comunicação com o CFS Mini; tente novamente", "level": "error", "cc": "FS3160"},
        3162: {"en": "CFSmini motor issue, try again.", "pt_BR": "Problema no motor do CFS Mini; tente novamente", "level": "warning", "cc": "FM3162"},
        3202: {"en": "Nozzle docking failed. The target nozzle station may be occupied. Please check the station status and try again.", "pt_BR": "Falha ao encaixar o bico: a estação de destino pode estar ocupada. Verifique a estação e tente novamente", "level": "warning", "cc": "TE3202"},
        3203: {"en": "Nozzle docking failed. Please check the printhead and nozzle station for anomalies and try again.", "pt_BR": "Falha ao encaixar o bico. Verifique anomalias no cabeçote e na estação de bicos e tente novamente", "level": "warning", "cc": "TE3203"},
        3204: {"en": "Nozzle docking failed. Please check the printhead and nozzle station for anomalies and try again.", "pt_BR": "Falha ao encaixar o bico. Verifique anomalias no cabeçote e na estação de bicos e tente novamente", "level": "warning", "cc": "TE3204"},
        3205: {"en": "Nozzle pickup failed. No nozzle available at the target station. Please check the station status and try again.", "pt_BR": "Falha ao pegar o bico: não há bico na estação de destino. Verifique a estação e tente novamente", "level": "warning", "cc": "TE3205"},
        3206: {"en": "Nozzle pickup failed. The nozzle was not grabbed. Please check whether the nozzle is placed correctly and try again.", "pt_BR": "Falha ao pegar o bico: o bico não foi agarrado. Verifique se ele está posicionado corretamente e tente novamente", "level": "warning", "cc": "TE3206"},
        3207: {"en": "Nozzle pickup failed. Please check the printhead and nozzle station for anomalies and try again.", "pt_BR": "Falha ao pegar o bico. Verifique anomalias no cabeçote e na estação de bicos e tente novamente", "level": "warning", "cc": "TE3207"},
        3208: {"en": "Nozzle pickup failed. Please check whether the nozzle is placed correctly and try again.", "pt_BR": "Falha ao pegar o bico. Verifique se ele está posicionado corretamente e tente novamente", "level": "warning", "cc": "TE3208"},
        3209: {"en": "Extruder clamp mechanism failed to open. Please check whether the clamp mechanism works properly and try again.", "pt_BR": "O mecanismo de fixação do extrusor não abriu. Verifique se funciona normalmente e tente novamente", "level": "warning", "cc": "TE3209"},
        3210: {"en": "The current nozzle diameter does not match the print task. Please check the print parameters and try again.", "pt_BR": "O diâmetro do bico atual não corresponde à tarefa de impressão. Verifique os parâmetros e tente novamente", "level": "warning", "cc": "TE3210"},
        3211: {"en": "Sensor fault detected: extruder filament runout / extruder hall / nozzle rack hall sensor.", "pt_BR": "Falha de sensor detectada: filamento do extrusor / hall do extrusor / hall da estação de bicos", "level": "warning", "cc": "TD3211"},
        3212: {"en": "Motion system restricted. A collision may have occurred!", "pt_BR": "Sistema de movimento restrito. Pode ter ocorrido uma colisão!", "level": "warning", "cc": "CM3212"},
        3213: {"en": "Nozzle calibration failed. Unable to obtain calibration sensor data.", "pt_BR": "Falha na calibração do bico: não foi possível obter dados do sensor de calibração", "level": "warning", "cc": "AD3213"},
        3214: {"en": "Nozzle calibration failed. An anomaly was detected in the detection area. Please make sure the nozzle is properly installed, clean the residue on the nozzle and heatbed surface, and try again.", "pt_BR": "Falha na calibração do bico: anomalia detectada na área de detecção. Confirme que o bico está bem instalado, limpe resíduos do bico e da mesa e tente novamente", "level": "warning", "cc": "AD3214"},
        3215: {"en": "Nozzle calibration failed. Abnormal detection signal. Please clean foreign objects on the heatbed surface, make sure the nozzle is not collided or loose, and try again.", "pt_BR": "Falha na calibração do bico: sinal de detecção anormal. Limpe objetos estranhos da mesa, confirme que o bico não bateu nem está solto e tente novamente", "level": "warning", "cc": "AD3215"},
        3216: {"en": "Nozzle calibration failed. Abnormal calibration data. Please perform nozzle calibration again.", "pt_BR": "Falha na calibração do bico: dados de calibração anormais. Refaça a calibração do bico", "level": "warning", "cc": "AD3216"},
        3217: {"en": "Nozzle calibration failed. Abnormal device movement, unable to complete calibration.", "pt_BR": "Falha na calibração do bico: movimento anormal do equipamento; não foi possível concluir a calibração", "level": "warning", "cc": "AD3217"},
        3221: {"en": "Nozzle pickup/drop-off coordinate calibration failed. Please check whether the machine is operating normally, manually mount the nozzle, and try again.", "pt_BR": "Falha na calibração das coordenadas de pegar/soltar o bico. Verifique se a máquina funciona normalmente, instale o bico manualmente e tente novamente", "level": "warning", "cc": "TE3221"},
        3231: {"en": "CFSNano communication issue, please try again after resolving.", "pt_BR": "Problema de comunicação com o CFS Nano; resolva e tente novamente", "level": "warning", "cc": "FS3231"},
        3234: {"en": "Instruction data error, transmission failed.", "pt_BR": "Erro nos dados da instrução; falha na transmissão", "level": "error", "cc": "FS3234"},
        3236: {"en": "Feed issue, Extruder filament sensor not triggered; filament not loaded into extruder.", "pt_BR": "Problema na alimentação: sensor de filamento do extrusor não acionado; o filamento não entrou no extrusor", "level": "warning", "cc": "FO3236"},
        3237: {"en": "Feed issue , filament may be jammed between the extruder sensor and the extruder gear. Please resolve the issue and click the \"Retry\" button.", "pt_BR": "Problema na alimentação: o filamento pode estar travado entre o sensor e a engrenagem do extrusor. Resolva e clique em \"Tentar novamente\"", "level": "warning", "cc": "FO3237"},
        3239: {"en": "No.{0} filament slot runs out, please insert filament in time.", "pt_BR": "O slot do filamento nº {0} está vazio; insira filamento a tempo", "level": "warning", "cc": "FR3239"},
        3240: {"en": "Command failed, current scenario does not support this command.", "pt_BR": "Comando falhou; o cenário atual não suporta este comando", "level": "error", "cc": "FS3240"},
        3244: {"en": "PTFE tube may have detached from the pneumatic fitting, please try again after resolving.", "pt_BR": "O tubo PTFE pode ter se soltado da conexão pneumática; resolva e tente novamente", "level": "warning", "cc": "FB3244"},
        3245: {"en": "Extruder may be clogged. Click \"Retry\" after resolving.", "pt_BR": "O extrusor pode estar entupido. Clique em \"Tentar novamente\" após resolver", "level": "warning", "cc": "FO3245"},
        3247: {"en": "Filament may tangled, please try again after resolving", "pt_BR": "O filamento pode estar emaranhado; resolva e tente novamente", "level": "warning", "cc": "FB3247"},
        3249: {"en": "Retract issue, filament cannot be returned to CFS hub. Please resolve the issue and click the \"Retry\" button.", "pt_BR": "Problema na retração: o filamento não consegue voltar ao hub do CFS. Resolva e clique em \"Tentar novamente\"", "level": "warning", "cc": "FR3249"},
        3251: {"en": "No.{0} RFID slot reading error, you can manually edit the filament.", "pt_BR": "Erro de leitura do RFID do slot nº {0}; você pode editar o filamento manualmente", "level": "error", "cc": "FS3251"},
        3252: {"en": "RFID key error, authentication failed. Please manually edit the filament information.", "pt_BR": "Erro de chave RFID; falha na autenticação. Edite as informações do filamento manualmente", "level": "error", "cc": "FS3252"},
        3253: {"en": "CFS Nano filament runout detection / odometry sensor fault.", "pt_BR": "Falha no detector de fim de filamento / sensor de odometria do CFS Nano", "level": "warning", "cc": "FD3253"},
        3254: {"en": "Extruder filament runout detection anomaly. The nozzle may not be clamped. Please check the nozzle status and try again.", "pt_BR": "Anomalia na detecção de filamento do extrusor. O bico pode não estar preso. Verifique o estado do bico e tente novamente", "level": "warning", "cc": "FD3254"},
        3255: {"en": "Extruder filament runout detection anomaly. Please check whether the filament is broken.", "pt_BR": "Anomalia na detecção de filamento do extrusor. Verifique se o filamento quebrou", "level": "warning", "cc": "FD3255"},
        3256: {"en": "Failed to write the filament slot remaining amount.", "pt_BR": "Falha ao gravar a quantidade restante de filamento no slot", "level": "error", "cc": "FS3256"},
        3263: {"en": "Retract issue, extruder filament sensor anomaly triggered, filament may have broken inside the extruder.", "pt_BR": "Problema na retração: sensor de filamento do extrusor acionado de forma anormal; o filamento pode ter quebrado dentro do extrusor", "level": "warning", "cc": "TR3263"},
        3266: {"en": "Retract issue, the filament may not have been cut. Please resolve the issue and try again.", "pt_BR": "Problema na retração: o filamento pode não ter sido cortado. Resolva e tente novamente", "level": "warning", "cc": "TR3266"},
        3280: {"en": "Air printing may have been detected.", "pt_BR": "Impressão no ar (sem extrusão) possivelmente detectada", "level": "warning", "cc": "FP3280"},
        5000: {"en": "Movement coordinates are out of range; please ensure the printing file is correct.", "pt_BR": "Coordenadas de movimento fora do limite; confirme que o arquivo de impressão está correto", "level": "warning", "cc": "CC5000"},
        5001: {"en": "Belt issue detected, loosen the belt and retry vibration pattern optimization", "pt_BR": "Problema na correia detectado; afrouxe a correia e repita a otimização do padrão de vibração", "level": "error", "cc": "CA5001"},
        5002: {"en": "Belt issue detected, tighten the belt and retry vibration pattern optimization", "pt_BR": "Problema na correia detectado; aperte a correia e repita a otimização do padrão de vibração", "level": "error", "cc": "CA5002"},
        5003: {"en": "Leveling sensor failure", "pt_BR": "Falha no sensor de nivelamento", "level": "warning", "cc": "CL5003"},
        5007: {"en": "Abnormal communication between cutter buffer module and host", "pt_BR": "Comunicação anormal entre o módulo de buffer do cortador e o host", "level": "warning", "cc": "FS5007"},
        5008: {"en": "Extruder clogging detected. Please troubleshoot and click the \"Retry\" button.", "pt_BR": "Entupimento do extrusor detectado. Diagnostique e clique em \"Tentar novamente\"", "level": "warning", "cc": "FO5008"},
        5009: {"en": "Feeding anomaly. The consumable may be clogged between the five-way connector and the consumable detector. Please troubleshoot the anomaly and click the \"Retry\" button.", "pt_BR": "Anomalia na alimentação: o filamento pode estar entupido entre o conector de cinco vias e o detector de filamento. Diagnostique e clique em \"Tentar novamente\"", "level": "warning", "cc": "FO5009"},
        5010: {"en": "Abnormal feeding. The consumables may be clogged between the consumable detector and the extruder material break detector. Please resolve the issue and click the \"Retry\" button.", "pt_BR": "Alimentação anormal: o filamento pode estar entupido entre o detector de filamento e o detector de quebra do extrusor. Resolva e clique em \"Tentar novamente\"", "level": "warning", "cc": "FO5010"},
        5011: {"en": "Abnormal RFID reading, RFID tag failed", "pt_BR": "Leitura RFID anormal; falha na etiqueta RFID", "level": "warning", "cc": "FS5011"},
        5012: {"en": "Abnormal RFID writing, RFID tag invalid", "pt_BR": "Gravação RFID anormal; etiqueta RFID inválida", "level": "warning", "cc": "FS5012"},
        5013: {"en": "The usage frequency of the cutter has reached the lifespan standard.", "pt_BR": "O cortador atingiu o limite de vida útil por frequência de uso", "level": "warning", "cc": "FC5013"},
        5014: {"en": "Cutting error at the tip of old material consumables", "pt_BR": "Erro de corte na ponta do filamento antigo", "level": "warning", "cc": "FO5014"},
        5015: {"en": "Failed to perform tip feeding excision of consumables. Please troubleshoot and click the \"Retry\" button.", "pt_BR": "Falha ao cortar a ponta do filamento na alimentação. Diagnostique e clique em \"Tentar novamente\"", "level": "warning", "cc": "FO5015"},
        5016: {"en": "Cutter initialization error", "pt_BR": "Erro de inicialização do cortador", "level": "warning", "cc": "FC5016"},
        5017: {"en": "Abnormality exists between the buffer and the filament detector of the extruder", "pt_BR": "Anormalidade entre o buffer e o detector de filamento do extrusor", "level": "warning", "cc": "FO5017"},
        5018: {"en": "Currently, the filament retraction during material change is abnormal, and the tip removal process fails to execute.", "pt_BR": "Retração anormal do filamento na troca de material; a remoção da ponta não foi executada", "level": "warning", "cc": "FO5018"},
        5019: {"en": "The communication module can interface communication is abnormal", "pt_BR": "Comunicação anormal na interface do módulo de comunicação CAN", "level": "warning", "cc": "XC5019"},
        5020: {"en": "Motherboard fan failure", "pt_BR": "Falha no ventilador da placa-mãe", "level": "error", "cc": "CF5020"},
        5021: {"en": "Home the XY axes before moving them", "pt_BR": "Faça o homing dos eixos XY antes de movê-los", "level": "error", "cc": "CC5021"},
        5022: {"en": "Home before moving the Z axis", "pt_BR": "Faça o homing antes de mover o eixo Z", "level": "error", "cc": "CZ5022"},
        5023: {"en": "The current RFID tag does not match the system. Please use Creality brand tagged consumables or replace with non-tagged consumables", "pt_BR": "A etiqueta RFID atual não corresponde ao sistema. Use filamento com etiqueta Creality ou troque por filamento sem etiqueta", "level": "warning", "cc": "FS5023"},
        5024: {"en": "Abnormal cutter reset, please handle it promptly", "pt_BR": "Reset anormal do cortador; resolva o quanto antes", "level": "warning", "cc": "FO5024"},
        5026: {"en": "Printhead vibration sensor failure", "pt_BR": "Falha no sensor de vibração do cabeçote", "level": "warning", "cc": "TO5026"},
        5027: {"en": "File does not exist", "pt_BR": "O arquivo não existe", "level": "error", "cc": "XD5027"},
        5028: {"en": "The current consumables are in a state where they cannot be retracted for material replacement. Please troubleshoot the issue and try again.", "pt_BR": "O filamento atual está em estado que impede a retração para troca de material. Resolva e tente novamente", "level": "warning", "cc": "FR5028"},
        5034: {"en": "Cutter displacement failure before cutting the tip of the consumables", "pt_BR": "Falha no deslocamento do cortador antes de cortar a ponta do filamento", "level": "warning", "cc": "FO5034"},
        5035: {"en": "Pre-loading error, please check if the material tray is jammed", "pt_BR": "Erro no pré-carregamento; verifique se a bandeja de material está travada", "level": "error", "cc": "FO5035"},
        5036: {"en": "The optical axis in the cutter buffer needs maintenance.", "pt_BR": "O eixo óptico do buffer do cortador precisa de manutenção", "level": "error", "cc": "FC5036"},
        5037: {"en": "CFS-C does not support this consumable for now. Please choose supported consumables.", "pt_BR": "O CFS-C ainda não suporta este filamento. Escolha um filamento compatível", "level": "error", "cc": "FS5037"},
        5038: {"en": "Flow calibration is enabled. In the current state, manually feed the material first, then start printing.", "pt_BR": "A calibração de fluxo está ativada. No estado atual, alimente o material manualmente antes de iniciar a impressão", "level": "error", "cc": "FS5038"},
        5510: {"en": "Heated bed temperature sensor failure", "pt_BR": "Falha no sensor de temperatura da mesa aquecida", "level": "warning", "cc": "CB5510"},
        5511: {"en": "Chamber temperature sensor failure", "pt_BR": "Falha no sensor de temperatura da câmara", "level": "error", "cc": "CT5511"},
        20000: {"en": "The file where the last unexpected stop was detected", "pt_BR": "Arquivo em que foi detectada a última parada inesperada", "level": "error"},
        20010: {"en": "Filament runout, printing paused, please refill", "pt_BR": "Filamento acabou, impressão pausada; recarregue o filamento", "level": "error"},
        30000: {"en": "No consumables detected, do you want to continue printing?", "pt_BR": "Nenhum filamento detectado; deseja continuar imprimindo?", "level": "error"},
        30010: {"en": "There is no consumables in the current rack", "pt_BR": "Não há filamento no suporte atual", "level": "error"},
        953231: {"en": "CFS nano disconnected. Check the cable connection, then tap \"Retry\".", "pt_BR": "CFS Nano desconectado. Verifique o cabo e toque em \"Tentar novamente\"", "level": "warning"},
        953234: {"en": "Instruction data error, transmission failed.", "pt_BR": "Erro nos dados da instrução; falha na transmissão", "level": "error"},
        953236: {"en": "Load error. Check if the spool or filament is jammed, then tap \"Retry\".", "pt_BR": "Erro ao carregar. Verifique se o carretel ou o filamento está travado e toque em \"Tentar novamente\"", "level": "warning"},
        953237: {"en": "Load error. Check for bent PTFE tube or filament jams, then tap \"Retry\".", "pt_BR": "Erro ao carregar. Verifique se o tubo PTFE está dobrado ou se há filamento travado e toque em \"Tentar novamente\"", "level": "warning"},
        953239: {"en": "No.{0} filament runs out, Insert filament, then tap \"Retry\".", "pt_BR": "O filamento nº {0} acabou. Insira filamento e toque em \"Tentar novamente\"", "level": "warning"},
        953244: {"en": "Load error. Check if the PTFE tube is detached, then tap \"Retry\".", "pt_BR": "Erro ao carregar. Verifique se o tubo PTFE se soltou e toque em \"Tentar novamente\"", "level": "warning"},
        953245: {"en": "Extruder clog detected. Clean the Hotend and check if the filament is slipping, then tap \"Retry\".", "pt_BR": "Entupimento do extrusor detectado. Limpe o hotend, verifique se o filamento está patinando e toque em \"Tentar novamente\"", "level": "warning"},
        953247: {"en": "Spool or filament tangle detected. Clear the tangle, then tap \"Retry\".", "pt_BR": "Carretel ou filamento emaranhado. Desfaça o nó e toque em \"Tentar novamente\"", "level": "warning"},
        953263: {"en": "Unload error. Filament may be broken inside the Extruder. Clear it, then tap \"Retry\".", "pt_BR": "Erro ao descarregar. O filamento pode ter quebrado dentro do extrusor. Retire-o e toque em \"Tentar novamente\"", "level": "warning"},
        "TE3213": {"en": "Nozzle docking failed. Multiple nozzle stations are empty. Please check the station status and try again.", "pt_BR": "Falha ao encaixar o bico: várias estações de bicos estão vazias. Verifique as estações e tente novamente", "level": "warning", "cc": "TE3213"}
    };

    // Hints per Creality code family (first two letters of cc): [en, pt_BR].
    const H = {
        "AC": ["Detected by the AI camera. Check the print, clear the plate/chute if needed, then resume or cancel.", "Detectado pela câmera de IA. Confira a peça, limpe a mesa/calha se preciso e retome ou cancele."],
        "AD": ["Nozzle calibration problem. Clean the nozzle and bed, check the nozzle is tight, then calibrate again.", "Problema na calibração do bico. Limpe o bico e a mesa, confirme o aperto do bico e calibre de novo."],
        "AL": ["AI LiDAR issue. Clean the LiDAR window and the bed; recalibrate if it persists.", "Problema no LiDAR de IA. Limpe a janela do LiDAR e a mesa; recalibre se persistir."],
        "BM": ["Mainboard/MCU or connection issue. Power-cycle the printer and check cables and cooling.", "Problema na placa-mãe/MCU ou nas conexões. Desligue e ligue a impressora e confira cabos e refrigeração."],
        "BS": ["Storage issue. Delete old files from the printer.", "Problema de armazenamento. Apague arquivos antigos da impressora."],
        "CA": ["Belt tension issue. Check/retension the belts and rerun Input Shaping.", "Problema na tensão das correias. Confira/retensione as correias e refaça o Input Shaping."],
        "CB": ["Heated bed temperature sensor or heating problem. Check the bed thermistor and wiring.", "Problema no sensor ou no aquecimento da mesa. Confira o termistor da mesa e a fiação."],
        "CC": ["Move or home error. Check the G-code limits and home the axes.", "Erro de movimento/homing. Confira os limites do G-code e faça o homing dos eixos."],
        "CF": ["Fan problem. Check that the fan spins freely and its connector is firm.", "Problema em ventilador. Confira se gira livremente e se o conector está firme."],
        "CL": ["Leveling/bed position issue. Check bed tilt and the leveling sensor, then retry.", "Problema de nivelamento/posição da mesa. Confira inclinação e sensor de nivelamento e tente de novo."],
        "CM": ["Motor/motion issue. Look for obstacles, check motor cables and speed limits.", "Problema de motor/movimento. Procure obstáculos e confira cabos dos motores e limites de velocidade."],
        "CP": ["Chamber heating issue. Keep the doors/lid closed and check the chamber heater fan.", "Problema no aquecimento da câmara. Mantenha portas/tampa fechadas e confira o ventilador do aquecedor da câmara."],
        "CT": ["Chamber temperature sensor issue. Check the thermistor and its wiring.", "Problema no sensor de temperatura da câmara. Confira o termistor e a fiação."],
        "CX": ["X axis problem. Check for obstructions, belt and endstop.", "Problema no eixo X. Verifique obstruções, correia e fim de curso."],
        "CY": ["Y axis problem. Check for obstructions, belt and endstop.", "Problema no eixo Y. Verifique obstruções, correia e fim de curso."],
        "CZ": ["Z axis problem. Check for obstructions, lead screws and bed level.", "Problema no eixo Z. Verifique obstruções, fusos e nível da mesa."],
        "FB": ["Filament buffer/feed path issue. Check for tangles and that the PTFE tube is seated, then retry.", "Problema no buffer/caminho do filamento. Verifique emaranhados e se o tubo PTFE está encaixado, depois tente de novo."],
        "FC": ["Cutter lifetime/maintenance. Service or replace the cutter.", "Vida útil/manutenção do cortador. Faça a manutenção ou troque o cortador."],
        "FD": ["Filament detection sensor issue. Check the filament path and sensors.", "Problema no sensor de detecção de filamento. Confira o caminho do filamento e os sensores."],
        "FF": ["CFS drying fan issue. Check the fan and try again.", "Problema no ventilador de secagem do CFS. Confira o ventilador e tente de novo."],
        "FH": ["CFS temperature/humidity sensor issue. Check the sensor connection.", "Problema no sensor de temperatura/umidade do CFS. Confira a conexão do sensor."],
        "FM": ["CFS feed motor issue. Check for jams and tangles.", "Problema no motor de alimentação do CFS. Verifique travamentos e emaranhados."],
        "FO": ["Filament feed/odometry issue. Check for a clog or jam in the path, clear it and press Retry.", "Problema de alimentação/odometria. Verifique entupimento ou travamento no caminho, desobstrua e tente novamente."],
        "FP": ["CFS drying/heating component issue. Check the device and try again.", "Problema em componente de aquecimento/secagem do CFS. Confira o equipamento e tente de novo."],
        "FR": ["Filament load/unload/runout issue. Check the spool and path, refill or clear the jam, then Retry.", "Problema de carga/descarga/fim de filamento. Confira o carretel e o caminho, recarregue ou desfaça o travamento e tente de novo."],
        "FS": ["CFS communication/RFID/system issue. Check cables and the CFS; edit the filament manually if needed.", "Problema de comunicação/RFID/sistema do CFS. Confira cabos e o CFS; edite o filamento manualmente se preciso."],
        "FT": ["CFS drying temperature issue. Check the device and environment.", "Problema na temperatura de secagem do CFS. Confira o equipamento e o ambiente."],
        "FV": ["CFS drying power supply issue. Check the power connection.", "Problema na fonte de secagem do CFS. Confira a alimentação."],
        "TC": ["Filament cutter issue. Check the cutter is free, not jammed, and calibrated.", "Problema no cortador de filamento. Confira se está livre, sem travar e calibrado."],
        "TD": ["Sensor fault in the toolhead. Check sensor cables.", "Falha de sensor no cabeçote. Confira os cabos dos sensores."],
        "TE": ["Nozzle/hotend issue. Check the nozzle thermistor, heater and nozzle seating.", "Problema no bico/hotend. Confira o termistor, o aquecedor e o encaixe do bico."],
        "TF": ["Toolhead fan too slow. Check the fan for dirt or an obstruction.", "Ventilador do cabeçote lento demais. Verifique sujeira ou obstrução."],
        "TO": ["Toolhead sensor failure. Check the sensor and its cable.", "Falha de sensor no cabeçote. Confira o sensor e o cabo."],
        "TR": ["Filament sensor triggered abnormally; the filament may have broken in the extruder. Clear it and retry.", "Sensor de filamento acionado de forma anormal; o filamento pode ter quebrado no extrusor. Retire e tente de novo."],
        "XC": ["Internal communication fault. Restart the printer.", "Falha de comunicação interna. Reinicie a impressora."],
        "XD": ["File problem. Re-slice and send the file again.", "Problema no arquivo. Refatie e envie o arquivo de novo."],
        "XS": ["System error. Restart the printer; update the firmware if it persists.", "Erro de sistema. Reinicie a impressora; atualize o firmware se persistir."]
    };

    // Creality only fills placeholders for these keys (CONCAT / CFS Pro drying / storage).
    const CFS_DRY = { 2870: 1, 2871: 1, 2872: 1, 2873: 1, 2874: 1 };
    const CAB = { L: ['Left', 'esquerda'], R: ['Right', 'direita'] };
    const MODE = { AUTO: ['Active Drying', 'secagem ativa'], ACTIVE: ['Auto Dehumidification', 'desumidificação automática'] };

    const CC = {};
    Object.keys(E).forEach(k => { if (E[k].cc && !CC[E[k].cc]) CC[E[k].cc] = k; });

    function valueArray(v) {
        if (Array.isArray(v)) return v;
        if (typeof v !== 'string' || !v.trim()) return null;
        try { const a = JSON.parse(v); return Array.isArray(a) ? a : null; } catch (e) { return null; }
    }

    function args(key, value, pt) {
        const a = valueArray(value);
        const i = pt ? 1 : 0;
        if (key === 508 && a && a.length > 1 && Number(a[1]) > 0) {      // [usedMB, totalMB, pct?]
            const u = Number(a[0]), t = Number(a[1]);
            let p = u / t * 100;
            const k = a.length > 2 ? Number(a[2]) : NaN;
            if (isFinite(k) && k > 0) p = k <= 1 ? k * 100 : (k >= 2 && k <= 100 ? k : p);
            return [(u / 1024).toFixed(1) + 'G/' + (t / 1024).toFixed(1) + 'G (' + p.toFixed(0) + '%)'];
        }
        if (key === 3239 || key === 3251) return a ? [String(a[0] === undefined ? '' : a[0]) + String(a[1] === undefined ? '' : a[1])] : [String(value || '')];
        if (key === 2939 || key === 953239) {                            // letters -> slot numbers, like Creality
            let s = '';
            String(a ? a.join('') : value || '').split('').forEach(c => { const U = c.toUpperCase(); s += U >= 'A' && U <= 'Z' ? U.charCodeAt(0) - 64 : c; });
            return [s];
        }
        if (CFS_DRY[key] && a) {
            const cab = CAB[String(a[1] || '').trim().toUpperCase()], mode = MODE[String(a[2] || '').trim().toUpperCase()];
            return [String(a[0] === undefined ? '' : a[0]), cab ? cab[i] : '', mode ? mode[i] : ''];
        }
        return null;
    }

    function fill(msg, list) {
        if (!list) return msg.replace(/\s*\{\d\}/g, '').trim();
        return msg.replace(/\{(\d)\}/g, (m, n) => (list[n] === undefined ? '' : list[n])).replace(/\s+/g, ' ').trim();
    }

    function lookup(err) {
        if (!err) return null;
        const hasKey = Object.prototype.hasOwnProperty.call(err, 'key');
        const key = Number(err.key);
        const str = typeof err.errcode === 'string' ? err.errcode.trim().toUpperCase() : '';
        if (str && !/^\d+$/.test(str) && CC[str]) return { id: CC[str], e: E[CC[str]] };
        if (key > 0) return { id: key, e: E[key] || null };
        const n = Number(err.errcode);
        if ((hasKey || n > 2) && n > 0) return { id: n, e: E[n] || null };            // K2 firmware: errcode mirrors key
        return null;                                                       // synthetic (Moonraker/Flashforge)
    }

    // Returns null when this is not a K2/Creality error, else
    // { code, text, hint, level: 'error'|'warning', known, creality } (pt_BR when Z.lang === 'pt').
    Z.k2Error = function (err) {
        const r = lookup(err);
        if (!r) return null;
        const pt = Z.lang === 'pt';
        const e = r.e;
        const key = Number(r.id);
        if (!e) return { code: String(r.id), text: '', hint: '', level: 'error', known: false, creality: '' };
        let text = fill(pt ? e.pt_BR : e.en, args(key, err.value, pt));
        if (key === 2839) { const a = valueArray(err.value); if (a && a.length) text = a.join('') + ' - ' + text; }
        const fam = e.cc ? e.cc.slice(0, 2) : '';
        const hint = fam && H[fam] ? H[fam][pt ? 1 : 0] : '';
        return { code: String(r.id), text: text, hint: hint, level: e.level, known: true, creality: e.cc || '' };
    };
    Z.K2_ERRORS = E;
})(window.Z = window.Z || {});
