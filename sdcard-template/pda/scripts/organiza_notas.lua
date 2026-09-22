-- organiza_notas.lua — gera notes/_indice.md com uma nota por linha.
-- Exemplo de "pequeno programa" útil rodando 100% no aparelho.

local notas = pda.fs.list("notes")
if not notas then
    pda.log("notes/ não acessível")
    return
end

local linhas = { "# Índice de notas", "" }
local total = 0
for _, nome in ipairs(notas) do
    if nome:sub(-4) == ".txt" and nome ~= "_indice.md" then
        local base = nome:sub(1, -5)
        local tamanho = pda.fs.size("notes/" .. nome) or 0
        table.insert(linhas, ("- %s (%d bytes)"):format(base, tamanho))
        total = total + 1
    end
end

local ok = pda.fs.write("notes/_indice.md", table.concat(linhas, "\n") .. "\n")
pda.log("índice com", total, "notas ->", ok and "ok" or "FALHOU")
pda.toast("Índice gerado: " .. total .. " notas")
