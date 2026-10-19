/* host_uigate_test.cpp — teste de host do UI-LOOP-GATE (main/ui_gate.h REAL).
 *
 * O gate existe por causa do assert xTaskToNotify==NULL (crash da 15ª rodada:
 * invoke_from_event_loop antes do run_event_loop — slint-esp.cpp v1.12.1
 * L132/L332/L336). Aqui o invoker é um MOCK que grava a ordem; a lógica pura
 * (deque + flag + mutex) é a mesma instanciada no firmware.
 *
 * Build (tools/hosttest/run.sh):
 *   g++ -std=c++17 -Wall -Wextra -pthread -I main \
 *       tools/hosttest/host_uigate_test.cpp -o /tmp/uigate_bin
 */
#include "ui_gate.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("[uigate] FALHA: %s (linha %d)\n", msg, __LINE__);          \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

/* Invoker mock: roda na hora e registra a ordem em g_order. */
static std::vector<int> g_order;
static void mock_invoke(std::function<void()> fn)
{
    fn();
}
using Gate = UiGate<void (*)(std::function<void()>)>;

static std::function<void()> rec(int id)
{
    return [id]() { g_order.push_back(id); };
}

/* ---- A: antes do flush, post acumula e NÃO chama o invoker ---- */
static void test_a(void)
{
    g_order.clear();
    Gate gate(&mock_invoke);
    CHECK(!gate.ready(), "A: gate nasce não-pronto");
    gate.post(rec(1));
    gate.post(rec(2));
    CHECK(g_order.empty(), "A: nada roda antes do flush");
    CHECK(gate.pending() == 2, "A: pending==2");
    printf("[uigate] A acumular-antes-do-loop ... ok\n");
}

/* ---- B: flush encaminha FIFO e devolve a contagem ---- */
static void test_b(void)
{
    g_order.clear();
    Gate gate(&mock_invoke);
    for (int i = 10; i <= 14; i++) gate.post(rec(i));
    int n = gate.flush();
    CHECK(n == 5, "B: flush devolve 5");
    CHECK(g_order == std::vector<int>({10, 11, 12, 13, 14}), "B: ordem FIFO");
    CHECK(gate.ready(), "B: pronto após flush");
    CHECK(gate.pending() == 0, "B: pending zerado");
    printf("[uigate] B flush-FIFO ... ok\n");
}

/* ---- C: flush é idempotente ---- */
static void test_c(void)
{
    g_order.clear();
    Gate gate(&mock_invoke);
    gate.post(rec(21));
    CHECK(gate.flush() == 1, "C: 1º flush roda 1");
    CHECK(gate.flush() == 0, "C: 2º flush não roda nada");
    CHECK(g_order.size() == 1, "C: evento não duplicado");
    printf("[uigate] C flush-idempotente ... ok\n");
}

/* ---- D: depois do flush, post é direto (síncrono) ---- */
static void test_d(void)
{
    g_order.clear();
    Gate gate(&mock_invoke);
    gate.flush();               /* sem pendências: só marca ready */
    gate.post(rec(31));
    CHECK(g_order == std::vector<int>({31}), "D: post pós-flush roda na hora");
    CHECK(gate.pending() == 0, "D: não volta p/ o pending");
    printf("[uigate] D post-direto-pos-loop ... ok\n");
}

/* ---- E: concorrência — produtores durante o flush; nada se perde ---- */
static void test_e(void)
{
    g_order.clear();
    Gate gate(&mock_invoke);
    std::atomic<int> total_exec {0};
    std::atomic<int> total_post {0};
    auto counted = [&total_exec](int) {
        return std::function<void()>([&total_exec]() { total_exec++; });
    };

    /* 500 eventos já acumulados antes do flush (como UAC/HID no bring-up) */
    for (int i = 0; i < 500; i++) {
        gate.post(counted(i));
        total_post++;
    }

    std::atomic<bool> stop {false};
    std::vector<std::thread> prod;
    for (int t = 0; t < 4; t++) {
        prod.emplace_back([&gate, &counted, &stop, &total_post, t]() {
            int i = 0;
            while (!stop.load()) {
                gate.post(counted(t * 100000 + (i++)));
                total_post++;
            }
        });
    }
    int n = gate.flush();               /* flush com produtores ativos */
    stop = true;
    for (auto &th : prod) th.join();
    /* Invariante real: NADA se perde e nada duplica — todo post ou foi
     * pelo lote do flush ou (gate já ready) direto ao invoker. */
    CHECK(n >= 500, "E: flush levou pelo menos os 500 pré-acumulados");
    CHECK(total_exec.load() == total_post.load(), "E: executados == postados");
    CHECK(gate.pending() == 0, "E: pending vazio no fim (gate ready)");
    /* Reposta a prova de perda: novos posts rodam na hora. */
    int before = total_exec.load();
    gate.post(counted(999));
    CHECK(total_exec.load() == before + 1, "E: post final executado");
    printf("[uigate] E concorrencia-sem-perda (%d posts, %d no flush) ... ok\n",
           total_exec.load(), n);
}

/* ---- F: post aninhado DENTRO do flush (evento que agenda evento) ---- */
static void test_f(void)
{
    g_order.clear();
    Gate gate(&mock_invoke);
    gate.post([&gate]() {              /* evento "pai" */
        g_order.push_back(41);
        gate.post(rec(42));            /* filho: gate já ready → direto */
    });
    gate.flush();
    CHECK(g_order == std::vector<int>({41, 42}), "F: pai→filho em ordem");
    printf("[uigate] F post-aninhado ... ok\n");
}

/* ---- G: cenário do firmware — driver task posta durante o bring-up,
 *      gate task dá flush depois; ordem de chegada preservada ---- */
static void test_g(void)
{
    g_order.clear();
    Gate gate(&mock_invoke);
    std::thread uac_evt([&gate]() {    /* evt_task: device connected */
        gate.post(rec(51));            /* push_mus_ui */
    });
    uac_evt.join();                    /* chegada ANTES da próxima */
    std::thread wifi_evt([&gate]() {   /* wifi task: status na barra */
        gate.post(rec(52));
    });
    wifi_evt.join();
    std::thread app_main_tail([&gate]() {
        gate.post(rec(53));            /* session_restore_apply */
    });
    app_main_tail.join();
    std::thread gate_task([&gate]() {  /* ui_gate: +100 ms */
        gate.flush();
    });
    gate_task.join();
    CHECK(g_order == std::vector<int>({51, 52, 53}),
          "G: UAC→Wi-Fi→restore na ordem de chegada");
    printf("[uigate] G cenario-firmware-15a-rodada ... ok\n");
}

int main(void)
{
    printf("=== host_uigate_test — UI-LOOP-GATE (ui_gate.h real) ===\n");
    test_a();
    test_b();
    test_c();
    test_d();
    test_e();
    test_f();
    test_g();
    if (g_fail) {
        printf("[uigate] %d FALHA(S)\n", g_fail);
        return 1;
    }
    printf("[uigate] A–G: TODOS OS TESTES PASSARAM\n");
    return 0;
}
