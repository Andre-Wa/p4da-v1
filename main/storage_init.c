/**
 * storage_init.c
 *
 * Camada de armazenamento do PDA.
 *
 *  - LittleFS interno  -> /internal   (sempre; fallback e espelho)
 *  - MicroSD (SDMMC)   -> /sdcard     (armazenamento PRINCIPAL)
 *  - Raiz lógica do sistema: <raiz>/pda  (ver pda_root())
 *
 * Correção importante em relação ao protótipo: o cartão agora sobe em
 * SDMMC 4-bit a 40 MHz (antes: 1-bit a 400 kHz, ~100x mais lento) e com
 * os pinos D2/D3 corretos da JC4880P443C_I_W (GPIO41/42; o GPIO45 do
 * protótipo é NC nesta placa).
 */

#include "storage_init.h"
#include "board_config.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

static const char *TAG = "storage";

static esp_ldo_channel_handle_t s_ldo_sd = NULL;
static sdmmc_card_t *s_card = NULL;
static bool s_sd_mounted = false;
static pda_store_t s_active = PDA_STORE_INTERNAL;

#define PDA_SUBDIRS "config", "scripts", "notes", "music", "logs", ".state"

/* ------------------------------------------------------------------ */
static esp_err_t init_internal_littlefs(void)
{
    ESP_LOGI(TAG, "montando LittleFS interno em /internal...");
    esp_vfs_littlefs_conf_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.base_path = "/internal";
    conf.partition_label = "storage";
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "falha ao montar LittleFS (%s)", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t sd_mount_try(const char *mount_point, int freq_khz, int width)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = freq_khz;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = width;
    slot.clk = BOARD_SD_CLK_GPIO;
    slot.cmd = BOARD_SD_CMD_GPIO;
    slot.d0 = BOARD_SD_D0_GPIO;
    slot.d1 = BOARD_SD_D1_GPIO;
    slot.d2 = BOARD_SD_D2_GPIO;
    slot.d3 = BOARD_SD_D3_GPIO;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.format_if_mount_failed = false; /* nunca formatar o cartão do usuário */
    mcfg.max_files = 8;
    mcfg.allocation_unit_size = 16 * 1024;

    return esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot, &mcfg, &s_card);
}

static esp_err_t sd_acquire_ldo(void)
{
    if (s_ldo_sd) return ESP_OK;
    ESP_LOGI(TAG, "ligando rail TF_VCC (LDO ch%d @ %d mV)...",
             BOARD_SD_LDO_CHAN, BOARD_SD_LDO_MV);
    esp_ldo_channel_config_t ldo_cfg;
    memset(&ldo_cfg, 0, sizeof(ldo_cfg));
    ldo_cfg.chan_id = BOARD_SD_LDO_CHAN;
    ldo_cfg.voltage_mv = BOARD_SD_LDO_MV;
    esp_err_t ret = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo_sd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "falha ao ligar LDO do SD: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t sd_mount_attempts_only(void);

static esp_err_t init_sdcard_sdmmc(void)
{
    esp_err_t ret = sd_acquire_ldo();
    if (ret != ESP_OK) return ret;

    return sd_mount_attempts_only();
}

static esp_err_t sd_mount_attempts_only(void)
{
    esp_err_t ret;
    /* Degraus de frequência/largura: rápido primeiro, degrada se o
     * cartão for problemático. 4-bit só usa D0..D3 reais desta placa. */
    struct { int freq; int width; const char *label; } attempts[] = {
        { SDMMC_FREQ_HIGHSPEED, 4, "4-bit @ 40 MHz" },
        { SDMMC_FREQ_DEFAULT,   4, "4-bit @ 20 MHz" },
        { SDMMC_FREQ_DEFAULT,   1, "1-bit @ 20 MHz" },
        { SDMMC_FREQ_PROBING,   1, "1-bit @ 400 kHz" },
    };

    for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
        ESP_LOGI(TAG, "tentando SD em %s...", attempts[i].label);
        ret = sd_mount_try("/sdcard", attempts[i].freq, attempts[i].width);
        if (ret == ESP_OK) {
            s_sd_mounted = true;
            ESP_LOGI(TAG, "MicroSD montado (%s)", attempts[i].label);
            return ESP_OK;
        }
        /* Nota: esp_vfs_fat_sdmmc_mount() já libera host/slot internamente
         * quando falha, então não há cleanup manual a fazer aqui. */
    }

    ESP_LOGW(TAG, "MicroSD ausente/ilegível — sistema roda no interno");
    s_sd_mounted = false;
    return ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
static void seed_skeleton(const char *root)
{
    char path[160];
    snprintf(path, sizeof(path), "%s", root);
    storage_ensure_dir(path);
    const char *subs[] = { PDA_SUBDIRS };
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, subs[i]);
        storage_ensure_dir(path);
    }
}

/* Promove configurações do interno p/ o cartão na primeira vez que o
 * cartão aparece com o interno já usado (evita "perder" settings). */
static void promote_settings_if_needed(void)
{
    char src[160], dst[160];
    snprintf(src, sizeof(src), "/internal/pda/config/system.lua");
    snprintf(dst, sizeof(dst), "/sdcard/pda/config/system.lua");
    if (storage_file_exists(src) && !storage_file_exists(dst)) {
        if (storage_copy_file(src, dst) == ESP_OK) {
            ESP_LOGI(TAG, "config promovida do interno para o cartão");
        }
    }
}

esp_err_t board_storage_init(void)
{
    init_internal_littlefs();
    init_sdcard_sdmmc();

    s_active = s_sd_mounted ? PDA_STORE_SD : PDA_STORE_INTERNAL;
    /* semeia SEMPRE as duas raízes: o espelho de config grava nas duas e
     * antes só a raiz ativa existia (E: "nao abriu /internal/pda/..."). */
    seed_skeleton("/internal/pda");
    if (s_sd_mounted) {
        seed_skeleton("/sdcard/pda");
        promote_settings_if_needed();
    }

    ESP_LOGI(TAG, "raiz ativa do sistema: %s", pda_root());
    return ESP_OK; /* ausência de cartão nunca trava o boot */
}

esp_err_t storage_remount_sd(void)
{
    if (s_sd_mounted && s_card) {
        ESP_LOGW(TAG, "unmount do SD p/ remontagem...");
        esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        s_card = NULL;
        s_sd_mounted = false;
    }
    esp_err_t err = sd_acquire_ldo();
    if (err == ESP_OK) err = sd_mount_attempts_only();
    if (err == ESP_OK) {
        s_active = PDA_STORE_SD;
        seed_skeleton("/sdcard/pda");
        ESP_LOGI(TAG, "SD remontado apos wake");
    } else {
        s_active = PDA_STORE_INTERNAL;
        ESP_LOGW(TAG, "SD nao remontou; raiz ativa -> /internal/pda");
    }
    return err;
}

void storage_shutdown_sd(void)
{
    if (!s_sd_mounted) return;
    ESP_LOGW(TAG, "desmontando SD p/ hibernação...");
    esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
    s_card = NULL;
    s_sd_mounted = false;
    if (s_ldo_sd) {
        esp_ldo_release_channel(s_ldo_sd);
        s_ldo_sd = NULL;
    }
}

/* ------------------------------------------------------------------ */
bool storage_sd_mounted(void) { return s_sd_mounted; }
pda_store_t storage_active_store(void) { return s_active; }

const char *pda_root(void)
{
    return (s_active == PDA_STORE_SD) ? "/sdcard/pda" : "/internal/pda";
}

void storage_sd_describe(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (!s_sd_mounted || !s_card) {
        snprintf(out, out_sz, "sem cartao");
        return;
    }
    /* capacidade em setores de 512 bytes */
    uint64_t bytes = (uint64_t)s_card->csd.capacity * (uint64_t)s_card->csd.sector_size;
    const char *kind = s_card->is_mmc ? "eMMC" : "SD";
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_sz, "%s %.1f GB", kind,
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        snprintf(out, out_sz, "%s %.0f MB", kind,
                 (double)bytes / (1024.0 * 1024.0));
    }
}

esp_err_t pda_path(char *out, size_t out_sz, const char *rel)
{
    if (!out || out_sz == 0) return ESP_ERR_INVALID_ARG;
    if (rel == NULL || rel[0] == '\0') {
        snprintf(out, out_sz, "%s", pda_root());
        return ESP_OK;
    }
    while (rel[0] == '/') rel++;
    int n = snprintf(out, out_sz, "%s/%s", pda_root(), rel);
    return (n > 0 && (size_t)n < out_sz) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* ------------------------------------------------------------------ */
esp_err_t storage_ensure_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir(%s) falhou: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t storage_read_text_file(const char *path, char *out_buf, size_t buf_size, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "não abriu %s (%s)", path, strerror(errno));
        return ESP_FAIL;
    }
    size_t n = fread(out_buf, 1, buf_size - 1, f);
    fclose(f);
    out_buf[n] = '\0';
    if (out_len) *out_len = n;
    return ESP_OK;
}

esp_err_t storage_read_file_alloc(const char *path, char **out_data, size_t *out_len)
{
    if (!out_data) return ESP_ERR_INVALID_ARG;
    *out_data = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ESP_FAIL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return ESP_FAIL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_data = buf;
    if (out_len) *out_len = n;
    return ESP_OK;
}

esp_err_t storage_write_text_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "não abriu %s p/ escrita (%s)", path, strerror(errno));
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    /* fflush/fsync antes de fechar: em SD, fclose sozinho pode deixar
     * dados no cache do FatFS se o rail cair logo depois. */
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "escreveu %u/%u bytes em %s", (unsigned)written, (unsigned)len, path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t storage_delete_file(const char *path)
{
    if (remove(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "falha ao apagar %s (%s)", path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool storage_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool storage_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t storage_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

esp_err_t storage_copy_file(const char *src, const char *dst)
{
    char *data = NULL;
    size_t len = 0;
    esp_err_t err = storage_read_file_alloc(src, &data, &len);
    if (err != ESP_OK) return err;
    err = storage_write_text_file(dst, data, len);
    free(data);
    return err;
}

esp_err_t storage_move_file(const char *src, const char *dst)
{
    if (storage_file_exists(dst) || storage_is_dir(dst)) {
        ESP_LOGW(TAG, "destino já existe: %s", dst);
        return ESP_ERR_INVALID_STATE;
    }
    if (rename(src, dst) == 0) return ESP_OK;   /* mesmo mount: rápido */
    esp_err_t err = storage_copy_file(src, dst);
    if (err != ESP_OK) return err;
    return storage_delete_file(src);
}

esp_err_t storage_rm_rf(const char *path)
{
    if (!storage_is_dir(path)) return storage_delete_file(path);
    DIR *d = opendir(path);
    if (!d) return ESP_FAIL;
    struct dirent *ent;
    char child[384];
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        storage_rm_rf(child);
    }
    closedir(d);
    if (rmdir(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "rmdir(%s): %s", path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}
