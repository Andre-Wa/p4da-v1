-- system.lua — configurações do PDA (JC4880P443C_I_W)
-- Este chunk precisa RETORNAR uma tabela. Edite à vontade no cartão;
-- o sistema reescreve este arquivo quando você salva pela tela Config.
-- Chaves ausentes assumem o default do firmware (ver docs/LUA.md).

return {
  display = {
    brightness = 80,            -- 0..100 (backlight PWM)
  },

  power = {
    dim_after_s = 30,           -- degrau 1: dimeriza o backlight
    screen_off_after_s = 120,   -- degrau 2: tela off + light sleep (wake: botão/toque)
    deep_sleep_after_s = 0,     -- degrau 3: hibernar após N s ocioso em standby (0 = nunca)
    wake_on_touch = false,      -- [HW?] usar INT do GT911 (GPIO21) como wake do standby
  },

  locale = {
    timezone = "America/Sao_Paulo",
    ntp_server = "pool.ntp.org",   -- usado a partir da fase Wi-Fi (M4)
  },

  ui = {
    onscreen_keyboard_auto = true, -- teclado virtual aparece só sem teclado USB
  },
}
