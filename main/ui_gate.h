/**
 * ui_gate.h — UI-LOOP-GATE (v5.16.0): invoke_from_event_loop seguro no bring-up.
 *
 * O PROBLEMA (crash reproduzido na 15ª rodada, boot pós-hibernação):
 *   slint::invoke_from_event_loop() ANTES de o event loop do Slint existir é
 *   assert NA CERTA. No backend ESP (slint-esp.cpp, tag v1.12.1 do slint-ui):
 *     - L336: `TaskHandle_t EspPlatform<PixelType>::task = {};`  → NULL;
 *     - L132: `task = xTaskGetCurrentTaskHandle();` só roda na 1ª linha de
 *             run_event_loop() (ou seja: só quando o loop JÁ começou);
 *     - L332: run_in_event_loop() — o que invoke_from_event_loop chama —
 *             termina em `vTaskNotifyGiveFromISR(task, nullptr)` INCONDICIONAL;
 *     - tasks.c do IDF v5.5.1 (vTaskGenericNotifyGiveFromISR):
 *             `configASSERT( xTaskToNotify );` → "assert failed:
 *              vTaskGenericNotifyGiveFromISR tasks.c:6171 (xTaskToNotify)".
 *   Qualquer callback de driver que dispare durante o bring-up do app_main
 *   (UAC device connected → push_mus_ui; HID connect; evento Wi-Fi; print do
 *   Lua) cai nesse buraco. O M4.15.3 já tinha esbarrado no MESMO assert pelo
 *   ramo noteedit do restore de sessão e adiou só aquele caminho; o buraco é
 *   estrutural — daqui em diante TUDO passa pelo gate.
 *
 * A SOLUÇÃO:
 *   UiGate::post(fn)  — thread-safe. Antes do flush(): fn é ACUMULADA (FIFO).
 *                       Depois: fn segue direto p/ o invoker real (invoke).
 *   UiGate::flush()   — idempotente; chama-se UMA vez quando o loop já está
 *                       de pé (task ui_gate, 100 ms após a criação da ui_loop,
 *                       que é prio 5 > main 1 e já executou a L132). O lote
 *                       acumulado é encaminhado ao invoker NA ORDEM e roda no
 *                       próprio event loop.
 *
 *   INV é injetável de propósito: no firmware é slint::invoke_from_event_loop;
 *   no teste de host (tools/hosttest/host_uigate_test.cpp) é um mock que grava
 *   a ordem — a lógica pura (deque + flag + mutex) nasce testada.
 */
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

template <typename INV>
class UiGate {
public:
    explicit UiGate(INV inv) : m_inv(std::move(inv)) {}
    UiGate(const UiGate &) = delete;
    UiGate &operator=(const UiGate &) = delete;

    /** Agenda fn no event loop. Seguro em QUALQUER thread e em qualquer
     *  momento do boot: antes do flush acumula, depois encaminha. */
    void post(std::function<void()> fn)
    {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (!m_ready) {
                m_pending.push_back(std::move(fn));
                return;
            }
        }
        m_inv(std::move(fn));
    }

    /** Marca o gate como pronto e encaminha o acumulado (FIFO) ao invoker.
     *  Idempotente: chamadas seguintes devolvem 0 e não tocam em nada.
     *  Deve rodar com o event loop do Slint JÁ iniciado. */
    int flush()
    {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_ready) return 0;
            m_ready = true;
            batch.swap(m_pending);
        }
        for (auto &fn : batch) m_inv(std::move(fn));
        return static_cast<int>(batch.size());
    }

    bool ready() const
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_ready;
    }
    size_t pending() const
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_pending.size();
    }

private:
    INV m_inv;
    mutable std::mutex m_mu;
    std::deque<std::function<void()>> m_pending;   /* protegido por m_mu */
    bool m_ready = false;                          /* protegido por m_mu */
};
