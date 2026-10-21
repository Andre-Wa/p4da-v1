-- ancoras_teste.lua — BAT.1d (v5.16.1): teste de descarga com janela manual
--
-- Fixa as âncoras do leitor de bateria em 3600-3700 mV:
--   * em carga / recém-desconectado (>= 3700 mV): o % trava em 100
--     ("ignora a carga alta");
--   * na bateria: o % só se move entre 3700 mV (100%) e 3600 mV (0%);
--   * o aprendizado de âncoras fica CONGELADO enquanto a janela manual
--     estiver ativa (um sag profundo não contamina o teste).
--
-- Confira sem serial: Config → Sistema mostra "ânc 3600-3700 mV (manual)".
-- O dado que importa p/ a v5.17.0 é o mV dessa linha (ao vivo, a cada 4 s):
-- anote mV × minuto desde a desconexão do USB. Abaixo de 3600 mV o % fica
-- em 0 até o fim — ESPERADO nesta janela estreita (a régua é o mV).
--
-- Para voltar ao normal: Config → Sistema → "Zerar calibração da bateria"
-- (ou rode um script com pda.battery.reset()).
-- Para testar OUTRA janela: edite os dois números abaixo — não precisa de
-- firmware novo (ex.: pda.battery.set(3400, 3900)).
local vmin, vmax = 3600, 3700

local ok = pda.battery.set(vmin, vmax)
if ok then
    pda.log("âncoras manuais ativas:", vmin, "-", vmax, "mV (aprendizado congelado)")
    pda.toast(string.format("âncoras %d-%d mV (manual)", vmin, vmax))
else
    pda.log("âncoras RECUSADAS (faixa inválida):", vmin, "-", vmax)
    pda.toast("âncoras recusadas")
end

local lo, hi, man = pda.battery.get()
pda.log("conferência pda.battery.get():", lo, hi, man)
