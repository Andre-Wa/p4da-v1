-- log_bateria.lua — BAT.1b (v5.16.2): logger de campo mV × minuto
--
-- A cada 60 s anexa uma linha em logs/bateria.csv (na raiz do pda):
--     t_min;uptime_s;hora;mv;pct;estado
-- É o dado da curva de descarga que alimenta a v5.17.0 (BAT.1b): ao
-- terminar, copie o arquivo p/ o PC e envie. Cada linha é fechada na hora
-- — se a bateria morrer, tudo até o último minuto já está no cartão.
--
-- Uso recomendado no teste da janela 3600-3700 mV:
--   1. rode ancoras_teste.lua uma vez (fixa a janela manual; persiste no
--      NVS e sobrevive a reboot);
--   2. rode ESTE script — pode ser antes ou depois de desconectar o USB:
--      ele só para em "carregando" DEPOIS de ter visto a bateria;
--   3. desconecte o USB e use o aparelho normalmente (brilho/Wi-Fi afetam
--      a descarga — o log registra o uso real, é isso que queremos);
--   4. ele PARA SOZINHO quando: o carregador volta (fim do teste), o
--      limite MAX_MIN passa, ou a gravação falha (cartão cheio/removido).
--      Parada antecipada = plugar o carregador.
--
-- Enquanto roda, a UI fica livre (o script tem task própria), mas NENHUM
-- outro script roda. Não teste com deep sleep ligado
-- (power.deep_sleep_after_s > 0 desliga o aparelho e mata o log).
-- A coluna "hora" só funciona com NTP sincronizado (Wi-Fi); sem ele fica
-- "-" e o eixo do tempo é o uptime_s.

local INTERVALO_S = 60     -- período de amostragem (s)
local MAX_MIN     = 600    -- limite de segurança: para sozinho após 10 h
local ARQ         = "logs/bateria.csv"

pda.fs.mkdir("logs")

local novo = not pda.fs.exists(ARQ)
if novo then
    local lo, hi, man = pda.battery.get()
    pda.fs.write(ARQ, "t_min;uptime_s;hora;mv;pct;estado\n")
    pda.fs.append(ARQ, string.format(
        "# janela %d-%d mV%s; script iniciou com uptime %d s\n",
        lo, hi, man and " (manual)" or "", pda.uptime()))
end

local t0 = pda.uptime()
local viu_bateria = false
local n = 0
pda.log(string.format(
    "log_bateria: gravando %s a cada %d s (limite %d min) — plugue o carregador p/ parar",
    ARQ, INTERVALO_S, MAX_MIN))

while true do
    local mv, pct, estado = pda.battery.read()
    local t = pda.uptime()
    local hora = "-"
    if os.time() > 1600000000 then hora = os.date("%H:%M:%S") end
    local linha = string.format("%d;%d;%s;%d;%d;%s\n",
        (t - t0) // 60, t, hora, mv, pct, estado)
    if not pda.fs.append(ARQ, linha) then
        pda.log("log_bateria: FALHA ao gravar (cartão cheio/saiu?) — encerrando")
        break
    end
    n = n + 1
    pda.log(string.format("log_bateria[%d] %s", n, (linha:gsub("\n", ""))))
    if estado ~= "carregando" then viu_bateria = true end
    if viu_bateria and estado == "carregando" then
        pda.log("log_bateria: carregador de volta — encerrando (teste completo)")
        break
    end
    if (t - t0) >= MAX_MIN * 60 then
        pda.log(string.format("log_bateria: limite de %d min atingido — encerrando",
            MAX_MIN))
        break
    end
    pda.sleep_ms(INTERVALO_S * 1000)
end

pda.log(string.format(
    "log_bateria: fim — %d amostra(s) em %s (copie do cartão e envie p/ a v5.17.0)",
    n, ARQ))
