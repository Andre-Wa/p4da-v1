-- config/wifi.lua — credenciais Wi-Fi (M4)
-- Sem este arquivo (ou com ssid vazio), o Wi-Fi não inicializa.
-- A senha fica em texto puro no cartão: trate-o como mídia sensível.
return {
  ssid = "",            -- ex.: "minha-rede"
  password = "",        -- ex.: "secreta"
  auto_connect = true,  -- reconecta sozinho em queda/acordar
}
