-- exemplo.lua — tour rápido pela API pda.* (toque nele na tela Scripts)
pda.log("olá do Lua!", pda.version())
pda.log("raiz ativa:", pda.root(), "| sd montado:", pda.sd_mounted())
pda.log("uptime (s):", pda.uptime())

-- config do sistema (leitura)
pda.log("brilho configurado:", pda.settings.get("display.brightness"))
pda.log("standby após (s):", pda.settings.get("power.screen_off_after_s"))

-- escrita/leitura de arquivo (caminho relativo à raiz ativa)
local ok = pda.fs.write("notes/from_lua.txt", "linha escrita por um script Lua\n")
pda.log("fs.write:", ok)
local conteudo = pda.fs.read("notes/from_lua.txt")
pda.log("fs.read:", conteudo)

-- listagem de diretório
local lista = pda.fs.list("scripts")
if lista then
    for i, nome in ipairs(lista) do
        pda.log("  script[" .. i .. "]:", nome)
    end
else
    pda.log("não consegui listar scripts/")
end

pda.toast("exemplo.lua terminou")
